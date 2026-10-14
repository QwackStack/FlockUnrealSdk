// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Containers/Ticker.h"
#include "FlockEvents.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockProviderBase.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Models/FlockAuthModels.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockSignInRulesTestHelpers
{
	/** A JWT for PlayerId; Variant changes the expiry so two tokens for one player differ. */
	inline FString SignInJwt(const FString& PlayerId, int32 Variant = 0)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600 + Variant;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/** The token routes answer at the root, not under `result`. */
	inline FString TokenReply(const FString& PlayerId, const FString& AccessToken, const FString& RefreshToken = TEXT("r-2"))
	{
		return FString::Printf(TEXT("{\"player_id\":\"%s\",\"access_token\":\"%s\",\"refresh_token\":\"%s\"}"),
			*PlayerId, *AccessToken, *RefreshToken);
	}

	inline FFlockRetryPolicy SignInRulesPolicy(int32 MaxRetries)
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = MaxRetries;
		Policy.InitialDelaySeconds = 0.f;
		return Policy;
	}

	inline void TickSignInRulesRetries()
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			FTSTicker::GetCoreTicker().Tick(1.f);
		}
	}

	/** Exposes the protected base so the rules are tested where they live. */
	class FSignInRulesProvider : public FFlockProviderBase
	{
	public:
		using FFlockProviderBase::FFlockProviderBase;
		using FFlockProviderBase::Execute;
		using FFlockProviderBase::FetchWithSnapshot;
		using FFlockProviderBase::ForTheCurrentSignInOnly;
		using FFlockProviderBase::SetAuthSession;
	};

	struct FSignInRulesFixture
	{
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		UFlockEvents* Events = nullptr;
		UFlockEventTestListener* Listener = nullptr;
		FSignInRulesProvider Provider;

		explicit FSignInRulesFixture(const FFlockRetryPolicy& Policy = SignInRulesPolicy(0))
			: Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
			, Provider(Client, Policy, MakeShared<FFlockNullLogger>())
		{
			Events = NewObject<UFlockEvents>();
			Listener = NewObject<UFlockEventTestListener>();
			Events->OnTokenRefreshed.AddDynamic(Listener, &UFlockEventTestListener::HandleTokenRefreshed);
			Events->OnAuthExpired.AddDynamic(Listener, &UFlockEventTestListener::HandleAuthExpired);
			Session->SetEvents(Events);
			Provider.SetAuthSession(Session);
		}

		void SignIn(const FString& PlayerId, int32 Variant = 0)
		{
			FString Error;
			Session->SetTokens(SignInJwt(PlayerId, Variant), TEXT("r-1"), Error);
		}

		/** GET /data through the base, headers read per attempt; bForTheSignIn opts it into the sign-in rule. */
		void Call(bool bForTheSignIn, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			const TSharedRef<FFlockHttpClient> ClientRef = Client;
			const TSharedRef<FFlockAuthSession> SessionRef = Session;
			FFlockRetryHandler::FOperation<FFlockPlayerLoginResponse> Send =
				[ClientRef, SessionRef](TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnAttempt)
				{
					return ClientRef->GetRaw<FFlockPlayerLoginResponse>(TEXT("http://x/v1/data"), SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
				};
			if (bForTheSignIn)
			{
				Send = Provider.ForTheCurrentSignInOnly<FFlockPlayerLoginResponse>(MoveTemp(Send));
			}
			Provider.Execute<FFlockPlayerLoginResponse>(MoveTemp(Send), MoveTemp(OnComplete), TEXT("Data call"));
		}

		/** Delivers held replies, and the requests those replies start, until nothing more is held. */
		void DeliverHeldReplies()
		{
			for (int32 Index = 0; Index < 6; ++Index)
			{
				Fake->FlushPending();
			}
		}
	};

	/** The body of every definition of Owner::Function in Text (a definition ends at the first `}` in column 0). */
	inline TArray<FString> DefinitionBodies(const FString& Text, const FString& Owner, const FString& Function)
	{
		TArray<FString> Bodies;
		const FString Signature = FString::Printf(TEXT("%s::%s("), *Owner, *Function);
		int32 From = 0;
		while (true)
		{
			const int32 Start = Text.Find(Signature, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (Start == INDEX_NONE)
			{
				break;
			}
			const int32 End = Text.Find(TEXT("\n}\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			if (End == INDEX_NONE)
			{
				break;
			}
			Bodies.Add(Text.Mid(Start, End - Start));
			From = End;
		}
		return Bodies;
	}

	inline bool LoadSdkSource(const FString& RelativeToPrivate, FString& OutText)
	{
		const FString TestsFolder = FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__)));
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(TestsFolder, TEXT(".."), RelativeToPrivate));
		if (!FFileHelper::LoadFileToString(OutText, *Path))
		{
			return false;
		}
		OutText.ReplaceInline(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
		return true;
	}
}

using namespace FlockSignInRulesTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInForbiddenTest, "Flock.Auth.SignIn.ForbiddenIsNeverAnsweredWithARefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInForbiddenTest::RunTest(const FString& Parameters)
{
	const FString NotHost = TEXT("{\"detail\":{\"code\":\"multiplayer.not_host\",\"message\":\"Only the host can perform this action\"}}");
	const TPair<FString, FString> Refusals[] = { { TEXT("coded"), NotHost }, { TEXT("bare"), TEXT("") } };
	for (const TPair<FString, FString>& Refusal : Refusals)
	{
		FSignInRulesFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TokenReply(TEXT("player-a"), SignInJwt(TEXT("player-a"), 1))));
		F.Fake->On(TEXT("v1/data"), FFlockFakeTransport::Status(403, Refusal.Value));

		FFlockError Error;
		F.Call(false, [&Error](TFlockResult<FFlockPlayerLoginResponse> R) { Error = R.Error; });

		TestEqual(FString::Printf(TEXT("%s 403: sent once"), *Refusal.Key), F.Fake->CountTo(TEXT("v1/data")), 1);
		TestEqual(FString::Printf(TEXT("%s 403: no token refresh"), *Refusal.Key), F.Fake->CountTo(TEXT("token/refresh")), 0);
		TestEqual(FString::Printf(TEXT("%s 403: the refusal is the answer"), *Refusal.Key), Error.StatusCode, 403);
		TestTrue(FString::Printf(TEXT("%s 403: still signed in"), *Refusal.Key), F.Session->IsAuthenticated());
	}

	// Control: a 401 is the lapsed sign-in a refresh is for.
	FSignInRulesFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TokenReply(TEXT("player-a"), SignInJwt(TEXT("player-a"), 1))));
	F.Fake->OnSequence(TEXT("v1/data"), { FFlockFakeTransport::Status(401, TEXT("")), FFlockFakeTransport::Ok(TokenReply(TEXT("p"), TEXT("t"))) });
	bool bSucceeded = false;
	F.Call(false, [&bSucceeded](TFlockResult<FFlockPlayerLoginResponse> R) { bSucceeded = R.bSuccess; });
	TestEqual(TEXT("401: one refresh"), F.Fake->CountTo(TEXT("token/refresh")), 1);
	TestEqual(TEXT("401: replayed once"), F.Fake->CountTo(TEXT("v1/data")), 2);
	TestTrue(TEXT("401: the replay succeeds"), bSucceeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInNumberTest, "Flock.Auth.SignIn.NumberMovesOnSignInAndSignOutOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInNumberTest::RunTest(const FString& Parameters)
{
	FSignInRulesFixture F;
	const int32 Start = F.Session->GetSignInNumber();
	F.SignIn(TEXT("player-a"));
	TestEqual(TEXT("a sign-in moves it"), F.Session->GetSignInNumber(), Start + 1);
	F.SignIn(TEXT("player-a"), 1);
	TestEqual(TEXT("signing in again, even as the same player, moves it"), F.Session->GetSignInNumber(), Start + 2);

	F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TokenReply(TEXT("player-a"), SignInJwt(TEXT("player-a"), 2))));
	bool bRefreshed = false;
	F.Session->RefreshAccessToken([&bRefreshed](bool bOk) { bRefreshed = bOk; });
	TestTrue(TEXT("the refresh was granted"), bRefreshed);
	TestEqual(TEXT("a refresh keeps it"), F.Session->GetSignInNumber(), Start + 2);

	F.Session->ClearTokens();
	TestEqual(TEXT("a sign-out moves it"), F.Session->GetSignInNumber(), Start + 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInRefreshAfterSignOutTest, "Flock.Auth.SignIn.RefreshAnsweredAfterSignOutChangesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInRefreshAfterSignOutTest::RunTest(const FString& Parameters)
{
	FSignInRulesFixture F;
	F.SignIn(TEXT("player-a"));
	F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TokenReply(TEXT("player-a"), SignInJwt(TEXT("player-a"), 1))));
	F.Fake->bDeferred = true;

	TOptional<bool> Refreshed;
	F.Session->RefreshAccessToken([&Refreshed](bool bOk) { Refreshed = bOk; });
	F.Session->ClearTokens();
	const int32 SavesAfterSignOut = F.Store->SaveCount;
	F.DeliverHeldReplies();

	TestTrue(TEXT("the waiting caller was answered"), Refreshed.IsSet());
	TestFalse(TEXT("answered as not refreshed"), Refreshed.Get(true));
	TestFalse(TEXT("still signed out"), F.Session->IsAuthenticated());
	TestTrue(TEXT("nothing saved for the next launch to restore"), F.Store->Stored.AccessToken.IsEmpty());
	TestEqual(TEXT("no save after the sign-out"), F.Store->SaveCount, SavesAfterSignOut);
	TestEqual(TEXT("no refreshed event"), F.Listener->TokenRefreshedCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInRefreshAfterSwitchTest, "Flock.Auth.SignIn.RefreshAnsweredAfterAnotherSignInChangesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInRefreshAfterSwitchTest::RunTest(const FString& Parameters)
{
	// Granted: player A's new tokens must not overwrite player B.
	{
		FSignInRulesFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TokenReply(TEXT("player-a"), SignInJwt(TEXT("player-a"), 1))));
		F.Fake->bDeferred = true;
		TOptional<bool> Refreshed;
		F.Session->RefreshAccessToken([&Refreshed](bool bOk) { Refreshed = bOk; });
		F.SignIn(TEXT("player-b"));
		const FString PlayerBToken = F.Session->GetAccessToken();
		F.DeliverHeldReplies();

		TestFalse(TEXT("granted: answered as not refreshed"), Refreshed.Get(true));
		TestEqualSensitive(TEXT("granted: player B's token is kept"), F.Session->GetAccessToken(), PlayerBToken);
		TestEqualSensitive(TEXT("granted: player B is signed in"), F.Session->GetPlayerId(), FString(TEXT("player-b")));
		TestEqual(TEXT("granted: no refreshed event"), F.Listener->TokenRefreshedCount, 0);
	}
	// Refused: player A's refusal must not sign player B out.
	{
		FSignInRulesFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Status(401, TEXT("")));
		F.Fake->bDeferred = true;
		F.Session->RefreshAccessToken([](bool) {});
		F.SignIn(TEXT("player-b"));
		const int32 ClearsBefore = F.Store->ClearCount;
		F.DeliverHeldReplies();

		TestTrue(TEXT("refused: player B is still signed in"), F.Session->IsAuthenticated());
		TestEqualSensitive(TEXT("refused: as player B"), F.Session->GetPlayerId(), FString(TEXT("player-b")));
		TestEqual(TEXT("refused: nothing cleared"), F.Store->ClearCount, ClearsBefore);
		TestEqual(TEXT("refused: no expired event"), F.Listener->AuthExpiredCount, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInUnauthorizedAfterSwitchTest, "Flock.Auth.SignIn.UnauthorizedAnsweredAfterASwitchRefreshesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInUnauthorizedAfterSwitchTest::RunTest(const FString& Parameters)
{
	auto Run = [](bool bSwitchPlayerMeanwhile, int32& OutDataRequests, int32& OutRefreshes)
	{
		FSignInRulesFixture F;
		F.SignIn(TEXT("player-a"));
		F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TokenReply(TEXT("player-a"), SignInJwt(TEXT("player-a"), 1))));
		F.Fake->OnSequence(TEXT("v1/data"), { FFlockFakeTransport::Status(401, TEXT("")), FFlockFakeTransport::Ok(TokenReply(TEXT("p"), TEXT("t"))) });
		F.Fake->bDeferred = true;
		F.Call(false, [](TFlockResult<FFlockPlayerLoginResponse>) {});
		if (bSwitchPlayerMeanwhile)
		{
			// Another player signs in with no sign-out between.
			F.SignIn(TEXT("player-b"));
		}
		F.DeliverHeldReplies();
		OutDataRequests = F.Fake->CountTo(TEXT("v1/data"));
		OutRefreshes = F.Fake->CountTo(TEXT("token/refresh"));
	};

	int32 DataRequests = 0;
	int32 Refreshes = 0;
	Run(false, DataRequests, Refreshes);
	TestEqual(TEXT("control: refreshed"), Refreshes, 1);
	TestEqual(TEXT("control: replayed"), DataRequests, 2);

	Run(true, DataRequests, Refreshes);
	TestEqual(TEXT("after a switch: player B's token is not refreshed for player A's refusal"), Refreshes, 0);
	TestEqual(TEXT("after a switch: not replayed as player B"), DataRequests, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInOwnRefreshTest, "Flock.Auth.SignIn.ANewSignInRefreshesForItself",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInOwnRefreshTest::RunTest(const FString& Parameters)
{
	FSignInRulesFixture F;
	F.SignIn(TEXT("player-a"));
	const FString PlayerBNewToken = SignInJwt(TEXT("player-b"), 9);
	F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Ok(TokenReply(TEXT("player-b"), PlayerBNewToken)));
	F.Fake->bDeferred = true;

	TOptional<bool> PlayerARefreshed;
	F.Session->RefreshAccessToken([&PlayerARefreshed](bool bOk) { PlayerARefreshed = bOk; });
	F.SignIn(TEXT("player-b"));
	TOptional<bool> PlayerBRefreshed;
	F.Session->RefreshAccessToken([&PlayerBRefreshed](bool bOk) { PlayerBRefreshed = bOk; });
	TestEqual(TEXT("player B's refresh is a request of its own"), F.Fake->CountTo(TEXT("token/refresh")), 2);

	F.DeliverHeldReplies();
	TestFalse(TEXT("player A's refresh is not used"), PlayerARefreshed.Get(true));
	TestTrue(TEXT("player B's refresh is"), PlayerBRefreshed.Get(false));
	TestEqualSensitive(TEXT("player B holds the new token"), F.Session->GetAccessToken(), PlayerBNewToken);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInPerCallTest, "Flock.Auth.SignIn.OnlyCallsForTheSignInStopAtASwitch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInPerCallTest::RunTest(const FString& Parameters)
{
	auto Run = [](bool bForTheSignIn, int32& OutRequests, EFlockErrorType& OutErrorType, bool bMadeSignedOut = false)
	{
		FSignInRulesFixture F(SignInRulesPolicy(2));
		if (!bMadeSignedOut)
		{
			F.SignIn(TEXT("player-a"));
		}
		F.Fake->OnSequence(TEXT("v1/data"), { FFlockFakeTransport::Status(503, TEXT("{}")), FFlockFakeTransport::Ok(TokenReply(TEXT("p"), TEXT("t"))) });
		bool bDone = false;
		F.Call(bForTheSignIn, [&](TFlockResult<FFlockPlayerLoginResponse> R)
		{
			bDone = true;
			OutErrorType = R.bSuccess ? EFlockErrorType::None : R.Error.Type;
		});
		F.SignIn(TEXT("player-b"));
		TickSignInRulesRetries();
		OutRequests = bDone ? F.Fake->CountTo(TEXT("v1/data")) : -1;
	};

	int32 Requests = 0;
	EFlockErrorType Type = EFlockErrorType::None;
	Run(true, Requests, Type);
	TestEqual(TEXT("a call for the sign-in: its retry is not sent"), Requests, 1);
	TestEqual(TEXT("a call for the sign-in: cancelled"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Cancelled));

	// A game-wide read is the same for every player, so it keeps retrying across the switch.
	Run(false, Requests, Type);
	TestEqual(TEXT("a game-wide call: its retry goes out"), Requests, 2);
	TestEqual(TEXT("a game-wide call: it succeeds"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::None));

	// Made with nobody signed in, it must not go out later carrying the token of whoever signs in meanwhile.
	Run(true, Requests, Type, /*bMadeSignedOut*/ true);
	TestEqual(TEXT("a call made signed out: its retry is not sent once someone signs in"), Requests, 1);
	TestEqual(TEXT("a call made signed out: cancelled"), static_cast<int32>(Type), static_cast<int32>(EFlockErrorType::Cancelled));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInCancelledNoCacheTest, "Flock.Auth.SignIn.ACancelledCallServesNoCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInCancelledNoCacheTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
		FString::Printf(TEXT("signin_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FSignInRulesFixture F;
	F.Provider.SetSnapshotStore(MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9")), TEXT("ver-1"));
	const TSharedRef<FFlockHttpClient> ClientRef = F.Client;
	auto Fetch = [&F, ClientRef](TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
	{
		return F.Provider.FetchWithSnapshot<FFlockPlayerLoginResponse>(TEXT("rules"), TEXT("key"),
			[ClientRef](TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnAttempt)
			{
				return ClientRef->GetRaw<FFlockPlayerLoginResponse>(TEXT("http://x/v1/cached"), TMap<FString, FString>(), MoveTemp(OnAttempt));
			},
			TEXT("Cached call"), MoveTemp(OnComplete));
	};

	// A first answer is kept.
	F.Fake->On(TEXT("v1/cached"), FFlockFakeTransport::Ok(TokenReply(TEXT("cached-player"), TEXT("t"))));
	Fetch([](TFlockResult<FFlockPlayerLoginResponse>) {});

	// Control: a server error with a cache serves the cache.
	F.Fake->On(TEXT("v1/cached"), FFlockFakeTransport::Status(503, TEXT("{}")));
	FString ServedPlayer;
	Fetch([&ServedPlayer](TFlockResult<FFlockPlayerLoginResponse> R) { ServedPlayer = R.bSuccess ? R.Value.PlayerId : FString(); });
	TestEqualSensitive(TEXT("control: a server error serves the cache"), ServedPlayer, FString(TEXT("cached-player")));

	// A cancelled call is told it was cancelled, not handed the cache. The engine answers a cancelled request this way.
	FFlockHttpResponse CancelledReply;
	CancelledReply.Result = EFlockHttpResult::Cancelled;
	F.Fake->On(TEXT("v1/cached"), CancelledReply);
	TOptional<TFlockResult<FFlockPlayerLoginResponse>> Answer;
	Fetch([&Answer](TFlockResult<FFlockPlayerLoginResponse> R) { Answer = R; });
	TestTrue(TEXT("the cancelled call was answered"), Answer.IsSet());
	if (Answer.IsSet())
	{
		TestFalse(TEXT("not with the cache"), Answer->bSuccess);
		TestEqual(TEXT("as cancelled"), static_cast<int32>(Answer->Error.Type), static_cast<int32>(EFlockErrorType::Cancelled));
	}
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSignInOptedInCallsTest, "Flock.Auth.SignIn.EveryCallThatActsForThePlayerKeepsItsSignIn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSignInOptedInCallsTest::RunTest(const FString& Parameters)
{
	struct FCalls
	{
		const TCHAR* File;
		const TCHAR* Owner;
		TArray<const TCHAR*> ActForThePlayer;
		TArray<const TCHAR*> GameWide;
	};
	// Each call that acts as the signed-in player through the token is sent only for its sign-in, with the token read on each
	// attempt; the game-wide calls beside them are controls that must not be.
	const FCalls Files[] = {
		{ TEXT("Providers/FlockNotificationProvider.cpp"), TEXT("FFlockNotificationProvider"),
			{ TEXT("GetNotifications"), TEXT("GetUnreadCount"), TEXT("GetSummary"), TEXT("MarkRead"), TEXT("MarkAllRead"),
			  TEXT("ScheduleInternal"), TEXT("CancelScheduled"), TEXT("GetScheduled"), TEXT("RegisterDeviceToken"), TEXT("UnregisterDeviceToken") },
			{ TEXT("GetTemplates") } },
		{ TEXT("Providers/FlockLeaderboardProvider.cpp"), TEXT("FFlockLeaderboardProvider"),
			{ TEXT("GetMyRank"), TEXT("GetAroundMe") }, { TEXT("GetStandings") } },
		{ TEXT("Providers/FlockAuthProvider.cpp"), TEXT("FFlockAuthProvider"),
			{ TEXT("SendEmailVerification"), TEXT("RevokeToken"), TEXT("GetLinkedAccounts"), TEXT("Unlink"), TEXT("VerifyEmail") },
			{ TEXT("IsNameAvailable"), TEXT("ForgotPassword"), TEXT("ResetPassword") } },
		{ TEXT("../Public/Providers/FlockAuthProvider.h"), TEXT("FFlockAuthProvider"), { TEXT("Link") }, {} },
		{ TEXT("Providers/FlockShopProvider.cpp"), TEXT("FFlockShopProvider"), { TEXT("Purchase") }, { TEXT("GetAll") } },
		// Every party route but "my party" goes through SendAs (pinned by Flock.Multiplayer.Parties.EveryRouteIsSentAsItsSignIn).
		{ TEXT("Multiplayer/FlockPartyRequests.cpp"), TEXT("FFlockPartyRequests"), { TEXT("SendAs"), TEXT("ReadMine") }, {} },
		// Every session route goes through SendAs (pinned by Flock.Multiplayer.Sessions.EveryRouteIsSentAsItsSignIn).
		{ TEXT("Multiplayer/FlockMultiplayerSessionRequests.cpp"), TEXT("FFlockMultiplayerSessionRequests"), { TEXT("SendAs") }, {} },
		{ TEXT("Providers/FlockMultiplayerProvider.cpp"), TEXT("FFlockMultiplayerProvider"), {}, { TEXT("ReadQueues") } },
	};

	auto KeepsItsSignIn = [](const FString& Body)
	{
		return Body.Contains(TEXT("ForTheCurrentSignInOnly"), ESearchCase::CaseSensitive)
			|| Body.Contains(TEXT("ForSignInOnly"), ESearchCase::CaseSensitive)
			|| Body.Contains(TEXT("/*bActsForThePlayer*/ true"), ESearchCase::CaseSensitive);
	};

	int32 Checked = 0;
	for (const FCalls& Calls : Files)
	{
		FString Text;
		if (!TestTrue(FString::Printf(TEXT("%s can be read"), Calls.File), LoadSdkSource(Calls.File, Text)))
		{
			continue;
		}
		for (const TCHAR* Function : Calls.ActForThePlayer)
		{
			const TArray<FString> Bodies = DefinitionBodies(Text, Calls.Owner, Function);
			const FString* Sending = Bodies.FindByPredicate([](const FString& Body)
			{
				return Body.Contains(TEXT("Execute<"), ESearchCase::CaseSensitive) || Body.Contains(TEXT("FetchAtScope<"), ESearchCase::CaseSensitive)
					|| Body.Contains(TEXT("FetchWithSnapshot<"), ESearchCase::CaseSensitive) || Body.Contains(TEXT("PostAccount<"), ESearchCase::CaseSensitive);
			});
			if (!TestNotNull(FString::Printf(TEXT("%s::%s sends a request"), Calls.Owner, Function), Sending))
			{
				continue;
			}
			TestTrue(FString::Printf(TEXT("%s::%s is sent only for its sign-in"), Calls.Owner, Function), KeepsItsSignIn(*Sending));
			TestFalse(FString::Printf(TEXT("%s::%s reads the token on each attempt, not once"), Calls.Owner, Function),
				Sending->Contains(TEXT("HeadersNow()"), ESearchCase::CaseSensitive));
			++Checked;
		}
		for (const TCHAR* Function : Calls.GameWide)
		{
			const TArray<FString> Bodies = DefinitionBodies(Text, Calls.Owner, Function);
			if (!TestTrue(FString::Printf(TEXT("%s::%s is found"), Calls.Owner, Function), Bodies.Num() > 0))
			{
				continue;
			}
			for (const FString& Body : Bodies)
			{
				TestFalse(FString::Printf(TEXT("%s::%s is game-wide and keeps retrying"), Calls.Owner, Function), KeepsItsSignIn(Body));
			}
		}
	}
	TestEqual(TEXT("every call that acts for the player was checked"), Checked, 22);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
