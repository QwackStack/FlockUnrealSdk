// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "FlockLocalPlayer.h"
#include "FlockSubsystem.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockHttpClient.h"
#include "Blueprint/FlockAuthAsyncActions.h"
#include "Blueprint/FlockCommandAsyncActions.h"
#include "Blueprint/FlockLeaderboardAsyncActions.h"
#include "Blueprint/FlockPlayerAsyncActions.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FlockLogger.h"
#include "Misc/Base64.h"
#include "Misc/FlockEngineCompat.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestRetries.h"
#include "Tests/Support/FlockTestSdk.h"
#include "UObject/UObjectGlobals.h"

#define FLOCK_LOCAL_PLAYERS_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.LocalPlayers." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockLocalPlayersTesting
{
	/** An access token that reads back as PlayerId for an hour. */
	inline FString TokenFor(const FString& PlayerId, const FString& Salt = FString())
	{
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld,\"n\":\"%s\"}"), *PlayerId,
			FDateTime::UtcNow().ToUnixTimestamp() + 3600, *Salt);
		FString Encoded = FBase64::Encode(Payload);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Encoded);
	}

	inline FFlockHttpResponse SignInAnswer(const FString& PlayerId, const FString& RefreshToken)
	{
		return FFlockFakeTransport::Ok(FString::Printf(TEXT("{\"player_id\":\"%s\",\"access_token\":\"%s\",\"refresh_token\":\"%s\"}"),
			*PlayerId, *TokenFor(PlayerId, RefreshToken), *RefreshToken));
	}

	inline FString Env(const FString& Inner)
	{
		return FString::Printf(TEXT("{\"error\":null,\"response\":null,\"result\":%s}"), *Inner);
	}

	inline FString StandingsBody()
	{
		return Env(TEXT("{\"window\":\"all\",\"total\":1,\"items\":[{\"rank\":1,\"player_id\":\"p1\",\"player_name\":\"Ada\",\"score\":10,")
			TEXT("\"country\":\"SA\",\"achieved_at\":\"\"}]}"));
	}

	inline FString EmptyPage()
	{
		return TEXT("{\"items\":[],\"total\":0,\"page\":1,\"limit\":10}");
	}

	inline FFlockInitConfig Config()
	{
		FFlockInitConfig Out;
		Out.ApiUrl = TEXT("https://api.example.com");
		Out.ApiKey = TEXT("key");
		Out.GameId = TEXT("Local Players Game");
		Out.GameVersion = TEXT("1.0.0");
		Out.GameVersionId = TEXT("ver-1");
		return Out;
	}

	/** The bearer a request carried, or empty. */
	inline FString BearerOf(const FFlockHttpRequest& Request)
	{
		const FString* Header = Request.Headers.Find(TEXT("Authorization"));
		return Header ? *Header : FString();
	}

	/** An initialized SDK over a fake transport, with the main player's tokens kept in memory. */
	struct FFixture
	{
		FFlockTestSdk Test;
		UFlockSubsystem* Sdk = Test.Sdk;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		UFlockEventTestListener* MainListener = NewObject<UFlockEventTestListener>();
		TStrongObjectPtr<UFlockEventTestListener> KeepMainListener{ MainListener };

		FFixture()
		{
			Sdk->SetHttpAdapterForTesting(Fake);
			Sdk->SetTokenStoreForTesting(Store);
			Sdk->GetEvents()->OnAuthenticated.AddDynamic(MainListener, &UFlockEventTestListener::HandleAuthenticated);
			Sdk->InitializeWithConfig(Config());
			Fake->On(FlockEndpoints::PlayerTokenRevoke, FFlockFakeTransport::Ok(TEXT("{\"revoked\":true}")));
			// "v1/player_data", not "player_data", which the command routes' addresses contain too.
			Fake->On(TEXT("v1/player_data"), FFlockFakeTransport::Ok(EmptyPage()));
		}

		/** Signs Local in with DeviceId, answered as PlayerId; returns the result. */
		TFlockResult<FFlockPlayerLoginResponse> SignIn(UFlockLocalPlayer* Local, const FString& DeviceId, const FString& PlayerId,
			const FString& RefreshToken)
		{
			Fake->On(FlockEndpoints::PlayerLoginDevice, SignInAnswer(PlayerId, RefreshToken));
			TFlockResult<FFlockPlayerLoginResponse> Out = TFlockResult<FFlockPlayerLoginResponse>::Fail(FFlockError());
			FFlockAuthProvider* Auth = Local ? Local->GetAuthProvider() : Sdk->GetAuthProvider();
			Auth->LoginWithDevice(DeviceId, [&Out](TFlockResult<FFlockPlayerLoginResponse> Result) { Out = Result; });
			return Out;
		}

		TFlockResult<FFlockPlayerDataPage> MyData(UFlockLocalPlayer* Local)
		{
			TFlockResult<FFlockPlayerDataPage> Out = TFlockResult<FFlockPlayerDataPage>::Fail(FFlockError());
			Local->GetPlayerProvider()->GetMyData(1, 10, [&Out](TFlockResult<FFlockPlayerDataPage> Result) { Out = Result; });
			return Out;
		}

		TFlockResult<FFlockTokenRevokeResponse> Remove(UFlockLocalPlayer* Local)
		{
			TFlockResult<FFlockTokenRevokeResponse> Out = TFlockResult<FFlockTokenRevokeResponse>::Fail(FFlockError());
			Sdk->RemoveLocalPlayer(Local, [&Out](TFlockResult<FFlockTokenRevokeResponse> Result) { Out = Result; });
			return Out;
		}
	};
}

using namespace FlockLocalPlayersTesting;

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersBeforeInitializeTest, "AddingBeforeFlockIsInitializedReturnsNull")
bool FFlockLocalPlayersBeforeInitializeTest::RunTest(const FString& Parameters)
{
	FFlockTestSdk Test;
	AddExpectedError(TEXT("AddLocalPlayer needs Flock initialized first"), EAutomationExpectedErrorFlags::Contains, 1);
	TestNull(TEXT("No local player before Flock is initialized"), Test->AddLocalPlayer());
	Test->InitializeWithConfig(Config());
	TestNotNull(TEXT("Counter-case: one once it is"), Test->AddLocalPlayer());
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersOwnSignInsTest, "TwoPlayersSignedInInTurnEachCallActsForItsOwnPlayer")
bool FFlockLocalPlayersOwnSignInsTest::RunTest(const FString& Parameters)
{
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	UFlockLocalPlayer* Omar = F.Sdk->AddLocalPlayer();
	UFlockEventTestListener* SaraListener = NewObject<UFlockEventTestListener>();
	Sara->GetEvents()->OnAuthenticated.AddDynamic(SaraListener, &UFlockEventTestListener::HandleAuthenticated);

	TestTrue(TEXT("Sara signs in"), F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara")).bSuccess);
	// The measured defect: Sara's read sent after Omar's sign-in carried Omar's sign-in.
	TestTrue(TEXT("Omar signs in"), F.SignIn(Omar, TEXT("typed-name:omar"), TEXT("p-omar"), TEXT("r-omar")).bSuccess);
	TestTrue(TEXT("Sara's read succeeds"), F.MyData(Sara).bSuccess);
	const FFlockHttpRequest SaraRead = F.Fake->LastRequest();
	TestTrue(TEXT("Sara's read is sent as Sara"), BearerOf(SaraRead).Contains(TokenFor(TEXT("p-sara"), TEXT("r-sara"))));
	TestTrue(TEXT("For Sara's player"), SaraRead.Url.Contains(TEXT("p-sara")));
	F.MyData(Omar);
	TestTrue(TEXT("Counter-case: Omar's read is sent as Omar"), BearerOf(F.Fake->LastRequest()).Contains(TokenFor(TEXT("p-omar"), TEXT("r-omar"))));

	TestEqualSensitive(TEXT("Each knows who they are"), Sara->GetPlayerId(), FString(TEXT("p-sara")));
	TestEqualSensitive(TEXT("Each knows who they are"), Omar->GetPlayerId(), FString(TEXT("p-omar")));
	TestFalse(TEXT("The main player is not signed in by either"), F.Sdk->IsAuthenticated());
	TestEqual(TEXT("Sara's sign-in is raised on her own events"), SaraListener->AuthenticatedCount, 1);
	TestEqual(TEXT("And never on the main hub, so analytics stays the main player's"), F.MainListener->AuthenticatedCount, 0);
	TestEqual(TEXT("No analytics session was started for a local player"), F.Fake->CountTo(TEXT("analytics/sessions")), 0);
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersDuplicateTest, "SigningInAsAPlayerAlreadySignedInHereIsRefusedAndTheSignInHereGetsTheNewTokens")
bool FFlockLocalPlayersDuplicateTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TestTrue(TEXT("The main player signs in"), F.SignIn(nullptr, TEXT("device-main"), TEXT("p-main"), TEXT("r-main-1")).bSuccess);
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();

	const TFlockResult<FFlockPlayerLoginResponse> Twice = F.SignIn(Sara, TEXT("device-main"), TEXT("p-main"), TEXT("r-main-2"));
	TestFalse(TEXT("Refused"), Twice.bSuccess);
	TestEqualSensitive(TEXT("With the SDK's code"), Twice.Error.Code, FString(TEXT("flock.player_already_signed_in")));
	TestTrue(TEXT("Typed for Blueprint"), Twice.Error.ErrorCode == EFlockErrorCode::FlockPlayerAlreadySignedIn);
	TestTrue(TEXT("Naming the player"), Twice.Error.Message.Contains(TEXT("p-main")));
	TestTrue(TEXT("And a fix"), Twice.Error.Hint.Contains(TEXT("RemoveLocalPlayer")));
	TestFalse(TEXT("The local player is not signed in"), Sara->IsAuthenticated());
	TestTrue(TEXT("The main player still is"), F.Sdk->IsAuthenticated());
	TestEqualSensitive(TEXT("And holds the pair the server now takes, saved too"), F.Store->Stored.RefreshToken, FString(TEXT("r-main-2")));

	// Two local players as one player.
	UFlockLocalPlayer* Omar = F.Sdk->AddLocalPlayer();
	TestTrue(TEXT("Counter-case: another player signs in"), F.SignIn(Omar, TEXT("device-omar"), TEXT("p-omar"), TEXT("r-omar-1")).bSuccess);
	const TFlockResult<FFlockPlayerLoginResponse> AsOmar = F.SignIn(Sara, TEXT("device-omar"), TEXT("p-omar"), TEXT("r-omar-2"));
	TestTrue(TEXT("A second local player as Omar is refused"), AsOmar.Error.ErrorCode == EFlockErrorCode::FlockPlayerAlreadySignedIn);
	TestTrue(TEXT("Omar stays signed in"), Omar->IsAuthenticated());

	// Omar's next renewal sends the pair handed over, which the server takes.
	F.Fake->On(FlockEndpoints::PlayerTokenRefresh, SignInAnswer(TEXT("p-omar"), TEXT("r-omar-3")));
	bool bRenewed = false;
	Omar->GetAuthProvider()->RefreshToken([&bRenewed](bool bOk) { bRenewed = bOk; });
	TestTrue(TEXT("Omar renews"), bRenewed);
	TestTrue(TEXT("With the handed-over token"), F.Fake->LastRequest().JsonBody.Contains(TEXT("r-omar-2")));
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersRefreshAfterHandOverTest, "ARefreshAnsweredAfterTheTokensWereHandedOverKeepsTheHandedOverPair")
bool FFlockLocalPlayersRefreshAfterHandOverTest::RunTest(const FString& Parameters)
{
	for (const bool bHandOver : { true, false })
	{
		FFixture F;
		UFlockLocalPlayer* Omar = F.Sdk->AddLocalPlayer();
		F.SignIn(Omar, TEXT("device-omar"), TEXT("p-omar"), TEXT("r-1"));
		// The server refuses r-1: another sign-in as Omar replaced it while the refresh was on its way.
		F.Fake->On(FlockEndpoints::PlayerTokenRefresh, FFlockFakeTransport::Coded(401, TEXT("player.invalid_refresh_token")));
		F.Fake->bDeferred = true;
		bool bRenewed = false;
		Omar->GetAuthProvider()->RefreshToken([&bRenewed](bool bOk) { bRenewed = bOk; });
		if (bHandOver)
		{
			UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
			F.Fake->bDeferred = false;
			F.SignIn(Sara, TEXT("device-omar"), TEXT("p-omar"), TEXT("r-2"));
			F.Fake->bDeferred = true;
		}
		F.Fake->FlushPending();
		if (bHandOver)
		{
			TestTrue(TEXT("The handed-over pair carries on"), Omar->IsAuthenticated() && bRenewed);
		}
		else
		{
			TestFalse(TEXT("Counter-case: with nothing handed over, the refusal ends the sign-in"), Omar->IsAuthenticated());
		}
	}
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersRemoveTest, "RemovingSignsOutAndEveryLaterCallSaysToAddANewOneWithoutSending")
bool FFlockLocalPlayersRemoveTest::RunTest(const FString& Parameters)
{
	FFixture F;
	AddExpectedError(TEXT("removed with RemoveLocalPlayer"), EAutomationExpectedErrorFlags::Contains, 0);
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	UFlockEventTestListener* SaraListener = NewObject<UFlockEventTestListener>();
	Sara->GetEvents()->OnLoggedOut.AddDynamic(SaraListener, &UFlockEventTestListener::HandleLoggedOut);
	F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara"));
	F.Fake->On(FlockEndpoints::LeaderboardStandings(TEXT("Board")), FFlockFakeTransport::Ok(StandingsBody()));
	TestTrue(TEXT("Counter-case: a board's standings go out while she is here"), [&]()
	{
		bool bOk = false;
		Sara->GetLeaderboardProvider()->GetStandings(TEXT("Board"), FFlockLeaderboardWindow::Current(), FString(), 1, 10,
			[&bOk](TFlockResult<FFlockStandings> Result) { bOk = Result.bSuccess; });
		return bOk;
	}());

	const TFlockResult<FFlockTokenRevokeResponse> Removed = F.Remove(Sara);
	TestTrue(TEXT("The sign-out answers"), Removed.bSuccess);
	TestEqual(TEXT("Signed out on the server"), F.Fake->CountTo(FlockEndpoints::PlayerTokenRevoke), 1);
	TestTrue(TEXT("As Sara"), BearerOf(F.Fake->LastRequest()).Contains(TokenFor(TEXT("p-sara"), TEXT("r-sara"))));
	TestFalse(TEXT("And here"), Sara->IsAuthenticated());
	TestTrue(TEXT("Who she was is forgotten"), Sara->GetPlayerId().IsEmpty());
	TestEqual(TEXT("Her own Logged Out is raised"), SaraListener->LoggedOutCount, 1);

	const int32 SentBefore = F.Fake->Requests.Num();
	const TFlockResult<FFlockPlayerDataPage> Read = F.MyData(Sara);
	TestFalse(TEXT("A call needing her sign-in fails"), Read.bSuccess);
	TestTrue(TEXT("Saying why"), Read.Error.Hint.Contains(TEXT("removed with RemoveLocalPlayer")));
	TestTrue(TEXT("And to add a new one"), Read.Error.Hint.Contains(TEXT("AddLocalPlayer")));
	TFlockResult<FFlockStandings> Standings = TFlockResult<FFlockStandings>::Fail(FFlockError());
	Sara->GetLeaderboardProvider()->GetStandings(TEXT("Board"), FFlockLeaderboardWindow::Current(), FString(), 1, 10,
		[&Standings](TFlockResult<FFlockStandings> Result) { Standings = Result; });
	TestFalse(TEXT("A call needing no sign-in fails too"), Standings.bSuccess);
	TestTrue(TEXT("With the same cause"), Standings.Error.Message.Contains(TEXT("removed with RemoveLocalPlayer")));
	TestTrue(TEXT("And fix"), Standings.Error.Hint.Contains(TEXT("AddLocalPlayer")));
	TestEqual(TEXT("And nothing is sent"), F.Fake->Requests.Num(), SentBefore);

	const TFlockResult<FFlockTokenRevokeResponse> Again = F.Remove(Sara);
	TestTrue(TEXT("Removing twice is refused"), !Again.bSuccess && Again.Error.Type == EFlockErrorType::Validation);
	TestFalse(TEXT("With a fix"), Again.Error.Hint.IsEmpty());
	TestFalse(TEXT("Null is refused the same way"), F.Remove(nullptr).bSuccess);
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersRetriesStopTest, "ARetryDueAfterRemovalIsNotSent")
bool FFlockLocalPlayersRetriesStopTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("removed with RemoveLocalPlayer"), EAutomationExpectedErrorFlags::Contains, 0);
	for (const bool bRemove : { true, false })
	{
		FFixture F;
		UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
		const FString Standings = FlockEndpoints::LeaderboardStandings(TEXT("Board"));
		F.Fake->OnSequence(Standings, { FFlockFakeTransport::Status(503, TEXT("{}")), FFlockFakeTransport::Ok(StandingsBody()) });
		FFlockError Failure;
		bool bDone = false;
		Sara->GetLeaderboardProvider()->GetStandings(TEXT("Board"), FFlockLeaderboardWindow::Current(), FString(), 1, 10,
			[&](TFlockResult<FFlockStandings> Result) { bDone = true; Failure = Result.Error; });
		TestEqual(TEXT("The first try went out"), F.Fake->CountTo(Standings), 1);
		if (bRemove)
		{
			F.Remove(Sara);
		}
		FlockTestRetries::Pump();
		if (bRemove)
		{
			TestEqual(TEXT("The retry due after removal is not sent"), F.Fake->CountTo(Standings), 1);
			TestTrue(TEXT("The call ends saying why"), bDone && Failure.Message.Contains(TEXT("removed with RemoveLocalPlayer")));
		}
		else
		{
			TestEqual(TEXT("Counter-case: without removal it is retried"), F.Fake->CountTo(Standings), 2);
		}
	}
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersLateSignInTest, "ASignInAnsweredAfterRemovalDoesNotSignThePlayerBackIn")
bool FFlockLocalPlayersLateSignInTest::RunTest(const FString& Parameters)
{
	for (const bool bRemove : { true, false })
	{
		FFixture F;
		UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
		UFlockEventTestListener* SaraListener = NewObject<UFlockEventTestListener>();
		Sara->GetEvents()->OnAuthenticated.AddDynamic(SaraListener, &UFlockEventTestListener::HandleAuthenticated);
		F.Fake->On(FlockEndpoints::PlayerLoginDevice, SignInAnswer(TEXT("p-sara"), TEXT("r-sara")));
		F.Fake->bDeferred = true;
		TFlockResult<FFlockPlayerLoginResponse> Late = TFlockResult<FFlockPlayerLoginResponse>::Fail(FFlockError());
		Sara->GetAuthProvider()->LoginWithDevice(TEXT("typed-name:sara"), [&Late](TFlockResult<FFlockPlayerLoginResponse> Result) { Late = Result; });
		if (bRemove)
		{
			F.Remove(Sara); // signed out still, so ended at once
		}
		F.Fake->FlushPending();
		if (bRemove)
		{
			TestFalse(TEXT("The late answer is refused"), Late.bSuccess);
			TestTrue(TEXT("Saying why"), Late.Error.Message.Contains(TEXT("removed with RemoveLocalPlayer")));
			TestTrue(TEXT("And to add a new one"), Late.Error.Hint.Contains(TEXT("AddLocalPlayer")));
			TestFalse(TEXT("She is not signed back in"), Sara->IsAuthenticated());
			TestEqual(TEXT("And no sign-in is raised"), SaraListener->AuthenticatedCount, 0);
		}
		else
		{
			TestTrue(TEXT("Counter-case: kept, the answer signs her in"), Late.bSuccess && Sara->IsAuthenticated());
		}
	}
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersShutdownTest, "ShuttingFlockDownEndsEveryLocalPlayerWithItsOwnCause")
bool FFlockLocalPlayersShutdownTest::RunTest(const FString& Parameters)
{
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	UFlockLocalPlayer* Omar = F.Sdk->AddLocalPlayer();
	F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara"));
	UFlockEventTestListener* SaraListener = NewObject<UFlockEventTestListener>();
	Sara->GetEvents()->OnLoggedOut.AddDynamic(SaraListener, &UFlockEventTestListener::HandleLoggedOut);
	UFlockEventTestListener* OmarListener = NewObject<UFlockEventTestListener>();
	Omar->GetEvents()->OnLoggedOut.AddDynamic(OmarListener, &UFlockEventTestListener::HandleLoggedOut);
	TestTrue(TEXT("Counter-case: signed in before"), Sara->IsAuthenticated());
	F.Sdk->ShutdownSdk();
	TestFalse(TEXT("Ended"), Sara->IsAuthenticated());
	TestEqual(TEXT("Her own Logged Out says so"), SaraListener->LoggedOutCount, 1);
	TestEqual(TEXT("Counter-case: nothing for a player who was not signed in"), OmarListener->LoggedOutCount, 0);
	const TFlockResult<FFlockPlayerDataPage> Read = F.MyData(Sara);
	TestTrue(TEXT("Saying Flock was shut down"), Read.Error.Hint.Contains(TEXT("Flock was shut down")));
	TestTrue(TEXT("And how to carry on"), Read.Error.Hint.Contains(TEXT("After initializing Flock again")));
	TestTrue(TEXT("A player who never signed in is ended too"), F.MyData(Omar).Error.Hint.Contains(TEXT("Flock was shut down")));
	F.Sdk->InitializeWithConfig(Config());
	TestFalse(TEXT("One added before is not this initialization's"), F.Remove(Sara).bSuccess);
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersNotSignedInTest, "ALocalPlayerNotSignedInYetIsToldToSignInThroughTheirOwnAuth")
bool FFlockLocalPlayersNotSignedInTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("Update player data"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Fetch player data"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	const TFlockResult<FFlockPlayerDataPage> Read = F.MyData(Sara);
	TestFalse(TEXT("Refused before she signs in"), Read.bSuccess);
	TestTrue(TEXT("Naming her own sign-in"), Read.Error.Hint.Contains(TEXT("Sign this local player in first")));

	// Online, her write reaches the server, whose refusal for want of a sign-in names her own sign-in.
	F.Fake->On(FlockEndpoints::CommandUpdatePlayerData, FFlockFakeTransport::Coded(401, TEXT("player.missing_token")));
	FFlockCommandData Score;
	Score.Set(TEXT("Score"), 1);
	TFlockResult<FFlockPlayerData> Write = TFlockResult<FFlockPlayerData>::Ok(FFlockPlayerData());
	Sara->GetCommandProvider()->UpdatePlayerData(TEXT("row-1"), Score, [&Write](TFlockResult<FFlockPlayerData> Result) { Write = Result; });
	TestFalse(TEXT("Her write is refused"), Write.bSuccess);
	TestTrue(TEXT("Naming her own sign-in"), Write.Error.Hint.Contains(TEXT("Sign this local player in first")));
	TestEqual(TEXT("Nor queued"), Sara->GetCommandProvider()->GetPendingWriteCount(), 0);

	// A call the server refuses for want of a sign-in gets the same fix.
	F.Fake->On(TEXT("v1/player_data"), FFlockFakeTransport::Coded(401, TEXT("player.missing_token")));
	FFlockError Refused;
	Sara->GetPlayerProvider()->GetDataById(TEXT("row-1"), [&Refused](TFlockResult<FFlockPlayerData> Result) { Refused = Result.Error; });
	TestTrue(TEXT("The server's refusal names her own sign-in"), Refused.Hint.Contains(TEXT("Sign this local player in first")));

	TFlockResult<FFlockPlayerDataPage> MainRead = TFlockResult<FFlockPlayerDataPage>::Fail(FFlockError());
	F.Sdk->GetPlayerProvider()->GetMyData(1, 10, [&MainRead](TFlockResult<FFlockPlayerDataPage> Result) { MainRead = Result; });
	TestEqualSensitive(TEXT("Counter-case: the main player keeps the general fix"), MainRead.Error.Hint, FFlockErrorHints::SignInFirst());
	FFlockError MainRefused;
	F.Sdk->GetPlayerProvider()->GetDataById(TEXT("row-1"), [&MainRefused](TFlockResult<FFlockPlayerData> Result) { MainRefused = Result.Error; });
	TestEqualSensitive(TEXT("Counter-case: and keeps the server refusal's own"), MainRefused.Hint,
		FFlockErrorHints::For(EFlockErrorCode::PlayerMissingToken));
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersSignedOutWriteTest, "AWriteWithNobodySignedInIsRefusedRatherThanQueuedForNoPlayer")
bool FFlockLocalPlayersSignedOutWriteTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("Update player data"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("could not reach the server"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	// The server is unreachable: a write would be queued.
	F.Fake->On(TEXT("leaderboard"), FFlockFakeTransport::Offline());
	F.Sdk->GetLeaderboardProvider()->GetStandings(TEXT("Board"), [](TFlockResult<FFlockStandings>) {});
	// Its retries run now, not in a later test.
	FlockTestRetries::Pump();
	TestTrue(TEXT("The main client is offline"), F.Sdk->IsLikelyOffline());
	FFlockCommandData Score;
	Score.Set(TEXT("Score"), 1);
	TFlockResult<FFlockPlayerData> Write = TFlockResult<FFlockPlayerData>::Ok(FFlockPlayerData());
	F.Sdk->GetCommandProvider()->UpdatePlayerData(TEXT("row-1"), Score, [&Write](TFlockResult<FFlockPlayerData> Result) { Write = Result; });
	TestFalse(TEXT("Signed out, the write is refused, not answered as done"), Write.bSuccess);
	TestEqualSensitive(TEXT("With the fix to sign in"), Write.Error.Hint, FFlockErrorHints::SignInFirst());
	TestEqual(TEXT("And nothing is queued"), F.Sdk->GetPendingCommandCount(), 0);

	F.Fake->On(FlockEndpoints::PlayerLoginDevice, SignInAnswer(TEXT("p-main"), TEXT("r-main")));
	F.Fake->On(FlockEndpoints::CommandUpdatePlayerData, FFlockFakeTransport::Offline());
	bool bSignedIn = false;
	F.Sdk->GetAuthProvider()->LoginWithDevice(TEXT("device-main"), [&bSignedIn](TFlockResult<FFlockPlayerLoginResponse> Result) { bSignedIn = Result.bSuccess; });
	TestTrue(TEXT("Signed in"), bSignedIn);
	F.Sdk->GetLeaderboardProvider()->GetStandings(TEXT("Board"), [](TFlockResult<FFlockStandings>) {});
	// Its retries run now, not in a later test.
	FlockTestRetries::Pump();
	F.Sdk->GetCommandProvider()->UpdatePlayerData(TEXT("row-1"), Score, [&Write](TFlockResult<FFlockPlayerData> Result) { Write = Result; });
	TestTrue(TEXT("Counter-case: signed in and offline, the write is queued for the player"), Write.bSuccess && F.Sdk->GetPendingCommandCount() == 1);
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersMainSignsInAsLocalTest, "TheMainPlayerSigningInAsAPlayerALocalPlayerHoldsEndsThatLocalPlayer")
bool FFlockLocalPlayersMainSignsInAsLocalTest::RunTest(const FString& Parameters)
{
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	UFlockLocalPlayer* Omar = F.Sdk->AddLocalPlayer();
	F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara-1"));
	F.SignIn(Omar, TEXT("typed-name:omar"), TEXT("p-omar"), TEXT("r-omar"));
	UFlockEventTestListener* SaraListener = NewObject<UFlockEventTestListener>();
	Sara->GetEvents()->OnLoggedOut.AddDynamic(SaraListener, &UFlockEventTestListener::HandleLoggedOut);
	TestTrue(TEXT("The main player signs in as Sara"), F.SignIn(nullptr, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara-2")).bSuccess);
	TestTrue(TEXT("And stays signed in"), F.Sdk->IsAuthenticated());
	TestFalse(TEXT("The local Sara is ended"), Sara->IsAuthenticated());
	TestEqual(TEXT("And her own Logged Out says so"), SaraListener->LoggedOutCount, 1);
	TestEqual(TEXT("Not signed out on the server, which would end the main player's sign-in"),
		F.Fake->CountTo(FlockEndpoints::PlayerTokenRevoke), 0);
	const TFlockResult<FFlockPlayerDataPage> Read = F.MyData(Sara);
	TestTrue(TEXT("Her calls say why"), Read.Error.Hint.Contains(TEXT("the main player signed in as the same player")));
	TestTrue(TEXT("And name the main player's auth"), Read.Error.Hint.Contains(TEXT("main player (the subsystem's GetAuthProvider")));
	TestTrue(TEXT("Counter-case: Omar, signed in as someone else, is untouched"), Omar->IsAuthenticated());
	TestFalse(TEXT("She is no longer one of the game's to remove"), F.Remove(Sara).bSuccess);
	F.Remove(Omar);
	TestEqual(TEXT("Counter-case: removing Omar signs him out on the server"), F.Fake->CountTo(FlockEndpoints::PlayerTokenRevoke), 1);
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersSignInWhileRemovingTest, "ASignInAnsweredWhileTheSignOutIsOnItsWayIsRefused")
bool FFlockLocalPlayersSignInWhileRemovingTest::RunTest(const FString& Parameters)
{
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara"));
	F.Fake->On(FlockEndpoints::PlayerLoginDevice, SignInAnswer(TEXT("p-omar"), TEXT("r-omar")));
	F.Fake->bDeferred = true;
	TFlockResult<FFlockPlayerLoginResponse> Late = TFlockResult<FFlockPlayerLoginResponse>::Fail(FFlockError());
	Sara->GetAuthProvider()->LoginWithDevice(TEXT("typed-name:omar"), [&Late](TFlockResult<FFlockPlayerLoginResponse> Result) { Late = Result; });
	bool bRemoved = false;
	F.Sdk->RemoveLocalPlayer(Sara, [&bRemoved](TFlockResult<FFlockTokenRevokeResponse>) { bRemoved = true; });
	TestTrue(TEXT("Counter-case: still signed in as herself while the sign-out is on its way"), Sara->IsAuthenticated());
	F.Fake->DeliverNext(); // the sign-in answers first
	TestFalse(TEXT("The sign-in answered meanwhile is refused"), Late.bSuccess);
	TestTrue(TEXT("Saying why"), Late.Error.Message.Contains(TEXT("removed with RemoveLocalPlayer")));
	TestEqualSensitive(TEXT("She is not signed in as someone else"), Sara->GetPlayerId(), FString(TEXT("p-sara")));
	F.Fake->FlushPending();
	TestTrue(TEXT("Then the sign-out answers and she is removed"), bRemoved && !Sara->IsAuthenticated());
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersOlderTokensTest, "TokensIssuedBeforeTheOnesASignInHoldsAreNotHandedToIt")
bool FFlockLocalPlayersOlderTokensTest::RunTest(const FString& Parameters)
{
	auto Issued = [](int64 IssuedAt, const FString& Salt)
	{
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"p-omar\",\"exp\":%lld,\"iat\":%lld,\"n\":\"%s\"}"),
			FDateTime::UtcNow().ToUnixTimestamp() + 3600, IssuedAt, *Salt);
		FString Encoded = FBase64::Encode(Payload);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Encoded);
	};
	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	TSharedRef<FFlockAuthSession> Session = MakeShared<FFlockAuthSession>(
		FFlockHttpClient::CreateDefault(5.f, MakeShared<FFlockUnrealLogger>(false)), nullptr, MakeShared<FFlockUnrealLogger>(false),
		TEXT("https://api.example.com/v1"), TMap<FString, FString>());
	FString Error;
	TestTrue(TEXT("Signed in with tokens issued now"), Session->SetTokens(Issued(Now, TEXT("b")), TEXT("r-newer"), Error));
	TestFalse(TEXT("Older tokens are not taken"), Session->KeepSignInWithNewTokens(Issued(Now - 5, TEXT("a")), TEXT("r-older"), Error));
	TestEqualSensitive(TEXT("It keeps its own"), Session->GetRefreshTokenValue(), FString(TEXT("r-newer")));
	TestTrue(TEXT("Counter-case: newer tokens are taken"), Session->KeepSignInWithNewTokens(Issued(Now + 5, TEXT("c")), TEXT("r-newest"), Error));
	TestEqualSensitive(TEXT("And held"), Session->GetRefreshTokenValue(), FString(TEXT("r-newest")));
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersOfflineCheckTest, "EachLocalPlayerAsksItsOwnConnectionWhetherTheServerIsReachable")
bool FFlockLocalPlayersOfflineCheckTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("could not reach the server"), EAutomationExpectedErrorFlags::Contains, 0);
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	UFlockLocalPlayer* Omar = F.Sdk->AddLocalPlayer();
	const FString Standings = FlockEndpoints::LeaderboardStandings(TEXT("Board"));
	auto ReadStandings = [](UFlockLocalPlayer* Local)
	{
		bool bOk = false;
		Local->GetLeaderboardProvider()->GetStandings(TEXT("Board"), FFlockLeaderboardWindow::Current(), FString(), 1, 10,
			[&bOk](TFlockResult<FFlockStandings> Result) { bOk = Result.bSuccess; });
		return bOk;
	};
	F.Fake->On(Standings, FFlockFakeTransport::Ok(StandingsBody()));
	TestTrue(TEXT("Standings read once, and kept for offline"), ReadStandings(Sara));

	// Sara's connection finds the server unreachable.
	F.Fake->On(Standings, FFlockFakeTransport::Offline());
	TestTrue(TEXT("Served from what was kept"), ReadStandings(Sara));
	TestFalse(TEXT("The main player's connection is not marked offline by hers"), F.Sdk->IsLikelyOffline());

	F.Fake->On(Standings, FFlockFakeTransport::Ok(StandingsBody()));
	const int32 Before = F.Fake->CountTo(Standings);
	TestTrue(TEXT("Omar reads"), ReadStandings(Omar));
	TestEqual(TEXT("Omar asks the server: his connection is not marked offline by hers"), F.Fake->CountTo(Standings), Before + 1);
	ReadStandings(Sara);
	TestEqual(TEXT("Counter-case: Sara's own is, so she is answered from what was kept without asking"), F.Fake->CountTo(Standings), Before + 1);
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersFreedTest, "ARemovedPlayerNobodyHoldsIsFreed")
bool FFlockLocalPlayersFreedTest::RunTest(const FString& Parameters)
{
	FFixture F;
	TWeakObjectPtr<UFlockLocalPlayer> Kept = F.Sdk->AddLocalPlayer();
	TWeakObjectPtr<UFlockLocalPlayer> Removed = F.Sdk->AddLocalPlayer();
	F.SignIn(Removed.Get(), TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara"));
	F.Remove(Removed.Get());
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("The removed player is freed"), Removed.IsValid());
	TestTrue(TEXT("Counter-case: one still added is kept while the game holds no reference"), Kept.IsValid());
	F.Sdk->ShutdownSdk();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("And freed once Flock shuts down"), Kept.IsValid());
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersNothingToRestoreTest, "ALocalPlayerHasNoSavedSignInToRestore")
bool FFlockLocalPlayersNothingToRestoreTest::RunTest(const FString& Parameters)
{
	FFixture F;
	F.SignIn(nullptr, TEXT("device-main"), TEXT("p-main"), TEXT("r-main"));
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara"));
	UFlockLocalPlayer* Omar = F.Sdk->AddLocalPlayer();
	bool bRestored = true;
	Omar->GetAuthProvider()->TryRestoreSession([&bRestored](bool bOk) { bRestored = bOk; });
	TestFalse(TEXT("Nothing is restored, not the main player's saved sign-in nor Sara's"), bRestored);
	TestFalse(TEXT("And he is not signed in"), Omar->IsAuthenticated());
	bool bMainRestored = false;
	F.Sdk->GetAuthProvider()->TryRestoreSession([&bMainRestored](bool bOk) { bMainRestored = bOk; });
	TestTrue(TEXT("Counter-case: the main player's saved sign-in restores"), bMainRestored);
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersBeingRemovedHoldsTest, "APlayerBeingRemovedStillHoldsTheirPlayerUntilTheSignOutAnswers")
bool FFlockLocalPlayersBeingRemovedHoldsTest::RunTest(const FString& Parameters)
{
	for (const bool bWhileRemoving : { true, false })
	{
		FFixture F;
		UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
		F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara-1"));
		F.Fake->bDeferred = true;
		F.Sdk->RemoveLocalPlayer(Sara, nullptr); // the sign-out waits
		if (!bWhileRemoving)
		{
			F.Fake->FlushPending();
		}
		F.Fake->bDeferred = false;
		UFlockLocalPlayer* Again = F.Sdk->AddLocalPlayer();
		const TFlockResult<FFlockPlayerLoginResponse> Second = F.SignIn(Again, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara-2"));
		if (bWhileRemoving)
		{
			TestTrue(TEXT("A local sign-in as her while her sign-out is on its way is refused"),
				Second.Error.ErrorCode == EFlockErrorCode::FlockPlayerAlreadySignedIn);
		}
		else
		{
			TestTrue(TEXT("Counter-case: once it answered, she signs in again"), Second.bSuccess && Again->IsAuthenticated());
		}
	}

	// The main player signing in as her while her sign-out is on its way ends her at once.
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	F.SignIn(Sara, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara-1"));
	F.Fake->bDeferred = true;
	bool bRemoved = false;
	F.Sdk->RemoveLocalPlayer(Sara, [&bRemoved](TFlockResult<FFlockTokenRevokeResponse>) { bRemoved = true; });
	F.Fake->bDeferred = false;
	F.SignIn(nullptr, TEXT("typed-name:sara"), TEXT("p-sara"), TEXT("r-sara-3"));
	TestFalse(TEXT("Ended by the main player's sign-in"), Sara->IsAuthenticated());
	const TFlockResult<FFlockPlayerDataPage> Read = F.MyData(Sara);
	TestTrue(TEXT("With that cause"), Read.Error.Hint.Contains(TEXT("the main player signed in as the same player")));
	F.Fake->FlushPending();
	TestTrue(TEXT("Her removal still completes"), bRemoved);
	TestTrue(TEXT("And the main player carries on"), F.Sdk->IsAuthenticated());
	return true;
}

FLOCK_LOCAL_PLAYERS_TEST(FFlockLocalPlayersNodesTest, "BlueprintNodesWithTheLocalPlayerPinActForThatPlayer")
bool FFlockLocalPlayersNodesTest::RunTest(const FString& Parameters)
{
	FFixture F;
	UFlockLocalPlayer* Sara = F.Sdk->AddLocalPlayer();
	// No World Context: a node left without the pin finds no Flock at all, so anything sent went through the pin.
	F.Fake->On(FlockEndpoints::PlayerLoginDevice, SignInAnswer(TEXT("p-sara"), TEXT("r-sara")));
	const int32 Before = F.Fake->Requests.Num();
	UFlockLoginAction::LoginWithDevice(nullptr, TEXT("typed-name:sara"))->Activate();
	TestEqual(TEXT("Counter-case: without the pin nothing is sent for her"), F.Fake->Requests.Num(), Before);
	UFlockLoginAction::LoginWithDevice(nullptr, TEXT("typed-name:sara"), Sara)->Activate();
	TestTrue(TEXT("Sign-in node: Sara is signed in"), Sara->IsAuthenticated());
	TestFalse(TEXT("And the main player is not"), F.Sdk->IsAuthenticated());
	const FString SaraBearer = TokenFor(TEXT("p-sara"), TEXT("r-sara"));

	UFlockGetMyPlayerDataAction::GetMyDataByTemplate(nullptr, TEXT("tpl-1"), Sara)->Activate();
	TestTrue(TEXT("Player node: sent as Sara"), BearerOf(F.Fake->LastRequest()).Contains(SaraBearer));
	FFlockCommandValue One(1);
	UFlockUpdatePlayerDataAction::UpdatePlayerDataField(nullptr, TEXT("row-1"), TEXT("Score"), One, Sara)->Activate();
	TestTrue(TEXT("Command node: sent as Sara"), F.Fake->LastRequest().Url.Contains(FlockEndpoints::CommandUpdatePlayerDataKey)
		&& BearerOf(F.Fake->LastRequest()).Contains(SaraBearer));
	const FString Standings = FlockEndpoints::LeaderboardStandings(TEXT("Board"));
	F.Fake->On(Standings, FFlockFakeTransport::Ok(StandingsBody()));
	UFlockGetLeaderboardStandingsAction::GetStandings(nullptr, TEXT("Board"), FFlockLeaderboardWindow::Current(), FString(), 1, 10, Sara)->Activate();
	TestEqual(TEXT("Leaderboard node: sent through her"), F.Fake->CountTo(Standings), 1);

	// The remove node needs the running game's Flock: only in a -game pass.
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World() != nullptr)
		{
			World = Context.World();
		}
	}
	UFlockSubsystem* GameFlock = World != nullptr ? UFlockSubsystem::Get(World) : nullptr;
	if (GameFlock == nullptr || !GameFlock->IsInitialized())
	{
		AddInfo(TEXT("No initialized Flock in a game world here: the remove node is checked in the -game pass."));
		return true;
	}
	UFlockLocalPlayer* Omar = GameFlock->AddLocalPlayer();
	UFlockRemoveLocalPlayerAction::RemoveLocalPlayer(World, Omar)->Activate();
	bool bRefused = false;
	GameFlock->RemoveLocalPlayer(Omar, [&bRefused](TFlockResult<FFlockTokenRevokeResponse> Result) { bRefused = !Result.bSuccess; });
	TestTrue(TEXT("Remove node: removed, so removing again is refused"), bRefused);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
