// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Config/FlockConfig.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Multiplayer/FlockMatchmaking.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Multiplayer/FlockParties.h"
#include "Multiplayer/FlockParty.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakeMatchmakingServer.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockPartyTestListener.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestSafeIndex.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "UObject/StrongObjectPtr.h"

namespace FlockMatchmakingTestHelpers
{
	using ERoute = FFlockFakeMatchmakingServer::ERoute;
	using FServer = FFlockFakeMatchmakingServer;

	const FString PlayerA = TEXT("player-a");
	const FString PlayerB = TEXT("player-b");
	const FString PlayerC = TEXT("player-c");

	/** A JWT naming PlayerId as its subject; the fake servers read the requester from it. */
	inline FString MatchmakingJwt(const FString& PlayerId)
	{
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId,
			FDateTime::UtcNow().ToUnixTimestamp() + 3600));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/** What a FindMatch call answered, how often, and the handle it returned; bAnswered stays false while it waits. */
	struct FSearchAnswer
	{
		bool bAnswered = false;
		int32 Answers = 0;
		TFlockResult<FFlockMatchmakingResult> Result;
		FFlockRequestHandle Handle;

		bool Succeeded() const { return bAnswered && Result.bSuccess; }
		bool FailedWith(EFlockErrorType Type) const { return bAnswered && !Result.bSuccess && Result.Error.Type == Type; }
		bool WasCancelled() const { return FailedWith(EFlockErrorType::Cancelled); }
		const FFlockMatchmakingResult& Value() const { return Result.Value; }
	};
	using FSearchAnswerRef = TSharedRef<FSearchAnswer>;

	inline FFlockOnMatchmakingResult Into(const FSearchAnswerRef& Answer, TFunction<void()> AlsoOnAnswer = nullptr)
	{
		return [Answer, AlsoOnAnswer](TFlockResult<FFlockMatchmakingResult> Result)
		{
			++Answer->Answers;
			Answer->bAnswered = true;
			Answer->Result = Result;
			if (AlsoOnAnswer)
			{
				AlsoOnAnswer();
			}
		};
	}

	/** What a party call answered, and its handle. */
	struct FPartyCall
	{
		bool bAnswered = false;
		TFlockResult<UFlockParty*> Result;
		FFlockRequestHandle Handle;

		bool Succeeded() const { return bAnswered && Result.bSuccess; }
		UFlockParty* Party() const { return Result.Value; }
	};
	using FPartyCallRef = TSharedRef<FPartyCall>;

	inline FFlockOnParty Into(const FPartyCallRef& Call)
	{
		return [Call](TFlockResult<UFlockParty*> Result)
		{
			Call->bAnswered = true;
			Call->Result = Result;
		};
	}

	inline FString Joined(const TArray<FString>& Ids) { return FString::Join(Ids, TEXT(",")); }

	inline TSharedPtr<FJsonObject> BodyOf(const FFlockHttpRequest& Request)
	{
		TSharedPtr<FJsonObject> Body;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
		return Body;
	}

	inline TSharedPtr<FJsonObject> JsonOf(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Json), Object);
		return Object;
	}

	inline int32 Containing(const TArray<FString>& Lines, const FString& Needle)
	{
		return Lines.FilterByPredicate([&](const FString& Line) { return Line.Contains(Needle, ESearchCase::CaseSensitive); }).Num();
	}

	/** The multiplayer stack as the provider builds it, over the matchmaking fake (with its party and session fakes), with time moved by hand. */
	struct FMatchmakingFixture
	{
		TSharedRef<double> Now = MakeShared<double>(1000.0);
		/** The jitter's roll for every wait drawn from now on: 0 the shortest (three quarters), 1 the longest. */
		TSharedRef<double> Roll = MakeShared<double>(0.5);
		TSharedRef<FServer> Server = MakeShared<FServer>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockAuthSession> Session;
		TSharedRef<FFlockMultiplayerProvider> Provider;
		TArray<TStrongObjectPtr<UFlockPartyTestListener>> Listeners;

		static FFlockRetryPolicy Retries(int32 MaxRetries)
		{
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = MaxRetries;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			return Policy;
		}

		explicit FMatchmakingFixture(double PartyRefreshSeconds = 0.0, int32 MaxRetries = 0)
			: Client(MakeShared<FFlockHttpClient>(Server, Log))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), Log, TEXT("http://x/v1"),
				TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") }, { TEXT("X-Game-Version-ID"), FFlockFakeSessionServer::GameBuild } }))
			, Provider(MakeShared<FFlockMultiplayerProvider>(Client, Retries(MaxRetries), Log, Session, TEXT("http://x/v1"), PartyRefreshSeconds))
		{
			const TSharedRef<double> Clock = Now;
			const TSharedRef<double> Rolled = Roll;
			Provider->GetRepeatingCalls().SetClockForTesting([Clock]() { return *Clock; }, [Rolled]() { return *Rolled; });
		}

		~FMatchmakingFixture()
		{
			Provider->Shutdown();
		}

		void SignIn(const FString& PlayerId)
		{
			FString Error;
			Session->SetTokens(MatchmakingJwt(PlayerId), TEXT("r-1"), Error);
		}

		void SignOut() { Session->ClearTokens(); }

		/** One frame of the provider: a search, party or session of an ended sign-in ends, then the calls due are sent. */
		void Frame() { Provider->TickForTesting(); }

		/** Moves time on and runs a frame; a check or refresh due by then is sent, and answered unless held. */
		void Advance(double Seconds)
		{
			*Now += Seconds;
			Frame();
		}

		/** Runs the retry handler's scheduled retries, which go through the core ticker, with no provider frame between. */
		static void PumpRetries()
		{
			for (int32 Index = 0; Index < 8; ++Index)
			{
				FTSTicker::GetCoreTicker().Tick(1.f);
			}
		}

		FFlockMatchmaking& Matchmaking() { return Provider->GetMatchmakingForTesting(); }
		UFlockMultiplayerSession* HeldSession() { return Provider->GetSessionsForTesting().GetHeld(); }
		UFlockParty* HeldParty() { return Provider->GetPartiesForTesting().GetHeld(); }

		FSearchAnswerRef Find(const FString& QueueName, const FFlockMatchmakingOptions& Options = FFlockMatchmakingOptions(),
			TFunction<void()> AlsoOnAnswer = nullptr)
		{
			const FSearchAnswerRef Answer = MakeShared<FSearchAnswer>();
			Answer->Handle = Provider->FindMatch(QueueName, Options, Into(Answer, MoveTemp(AlsoOnAnswer)));
			return Answer;
		}

		FSearchAnswerRef FindAsParty(UFlockParty* Party, const FString& QueueName, TFunction<void()> AlsoOnAnswer = nullptr)
		{
			const FSearchAnswerRef Answer = MakeShared<FSearchAnswer>();
			if (Party != nullptr)
			{
				Answer->Handle = Party->FindMatch(QueueName, FFlockMatchmakingOptions(), Into(Answer, MoveTemp(AlsoOnAnswer)));
			}
			return Answer;
		}

		FPartyCallRef GetMine()
		{
			const FPartyCallRef Call = MakeShared<FPartyCall>();
			Call->Handle = Provider->GetMyParty(Into(Call));
			return Call;
		}

		FPartyCallRef Join(const FString& Code)
		{
			const FPartyCallRef Call = MakeShared<FPartyCall>();
			Call->Handle = Provider->JoinParty(Code, Into(Call));
			return Call;
		}

		/** The player's party as GetMyParty answers it, or null with the test failed. */
		UFlockParty* MyParty(FAutomationTestBase& Test)
		{
			const FPartyCallRef Mine = GetMine();
			Test.TestTrue(TEXT("precondition: the party was read"), Mine->Succeeded() && Mine->Party() != nullptr);
			return Mine->Party();
		}

		UFlockPartyTestListener* Listen(UFlockParty* Party)
		{
			UFlockPartyTestListener* Listener = NewObject<UFlockPartyTestListener>();
			Listeners.Emplace(Listener);
			if (Party != nullptr)
			{
				Listener->Listen(Party);
			}
			return Listener;
		}

		/** A party another player made, with the given members joined after them, on the server directly. */
		FString PartyOf(const FString& Leader, const TArray<FString>& Others)
		{
			const FString PartyId = Server->Parties->MadeBy(Leader).Id;
			for (const FString& Other : Others)
			{
				Server->Parties->Joins(PartyId, Other);
			}
			return PartyId;
		}

		FFlockFakePartyServer::FParty* PartyById(const FString& PartyId) { return Server->Parties->PartyById(PartyId); }

		/** The player's latest ticket on the server. */
		TSharedPtr<FServer::FTicket> TicketOf(const FString& PlayerId) const
		{
			const TArray<FServer::FTicketRef> Tickets = Server->TicketsOf(PlayerId);
			return Tickets.Num() > 0 ? TSharedPtr<FServer::FTicket>(Tickets.Last()) : TSharedPtr<FServer::FTicket>();
		}

		FString StatusOf(const FString& PlayerId) const
		{
			const TSharedPtr<FServer::FTicket> Ticket = TicketOf(PlayerId);
			return Ticket.IsValid() ? Ticket->Status : FString(TEXT("(no ticket)"));
		}
	};

	/** The real subsystem, initialized over the matchmaking fake and signed in as PlayerA, with analytics off so only multiplayer talks to it. */
	struct FSubsystemFixture
	{
		TSharedRef<FServer> Server = MakeShared<FServer>();
		FFlockTestSdk Test;
		bool bAnalyticsWas = false;

		FSubsystemFixture()
		{
			UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
			bAnalyticsWas = Settings->bAnalyticsEnabled;
			Settings->bAnalyticsEnabled = false;
			const TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
			Store->Stored.AccessToken = MatchmakingJwt(PlayerA);
			Store->Stored.RefreshToken = TEXT("r-1");
			Store->bHasTokens = true;
			Test->SetHttpAdapterForTesting(Server);
			Test->SetTokenStoreForTesting(Store);
			FFlockInitConfig Config;
			Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
			Config.ApiKey = TEXT("secret");
			Config.GameId = TEXT("my-game");
			Config.GameVersion = TEXT("1.2.3");
			Config.GameVersionId = FFlockFakeSessionServer::GameBuild;
			Test->InitializeWithConfig(Config);
		}

		~FSubsystemFixture()
		{
			GetMutableDefault<UFlockConfig>()->bAnalyticsEnabled = bAnalyticsWas;
		}

		FFlockMultiplayerProvider* Provider() const { return Test->GetMultiplayerProvider(); }
	};

	// Real answers from the backend, captured by the matchmaking rules probe on 2026-10-08, nulls included.
	const TCHAR* MatchedRead = TEXT(R"BODY({"id":"01M4DD484KEYM8KCGGZZMK7S3S","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2SEP5GZ15W8TV9N2Q6MY","status":"matched","attributes":{},"party_id":null,"party_size":1,"match_id":"01M4DD4GAJB0W526MC8ZJ1X3ZQ","matched_at":"2026-10-08T09:22:29.324253","cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:20.947180","match":{"id":"01M4DD4GAJB0W526MC8ZJ1X3ZQ","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","status":"ready","player_ids":["01M46J2QM0KSQ1X3QNF03RRT6V","01M46J2QXJHZQPWEVG4AJX6MMF","01M46J2SEP5GZ15W8TV9N2Q6MY"],"connection_info":{"mode":"p2p","session_id":"01M4DD4GAPBE2N9FDZVVWJFFYT"},"created_at":"2026-10-08T09:22:29.330408"}})BODY");
	const TCHAR* ExpiredRead = TEXT(R"BODY({"id":"01M4DD5QM53B9PVPJX2K654X45","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J4VCTQ54NSPP206GSA069","status":"expired","attributes":{},"party_id":null,"party_size":1,"match_id":null,"matched_at":null,"cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:23:09.573881","match":null})BODY");
	const TCHAR* CancelAnswer = TEXT(R"BODY({"id":"01M4DD3MJ2WEPBZHMR6B46RH6B","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2QM0KSQ1X3QNF03RRT6V","status":"cancelled","attributes":{},"party_id":null,"party_size":1,"match_id":null,"matched_at":null,"cancel_reason":"player","game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:00.898408"})BODY");
	const TCHAR* PartyTicketsMade = TEXT(R"BODY({"error":{"code":null},"response":{"message":null,"code":null},"result":[{"id":"01M4DD403BPAZX7JV58263WCP0","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2QM0KSQ1X3QNF03RRT6V","status":"queued","attributes":{},"party_id":"01M4DD3Z5XA9NQJ74SBD1BX698","party_size":2,"match_id":null,"matched_at":null,"cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:12.715871"},{"id":"01M4DD403C8AVYGYQKZ87Y7BKQ","queue_id":"01M46J2MG533AXD49SBPTEPBRM","game_id":"01KVCSANMD9V0CRW85ADE1WJA1","player_id":"01M46J2QXJHZQPWEVG4AJX6MMF","status":"queued","attributes":{},"party_id":"01M4DD3Z5XA9NQJ74SBD1BX698","party_size":2,"match_id":null,"matched_at":null,"cancel_reason":null,"game_version_id":"01KVCSB5ND68QBATA4468M0APS","created_at":"2026-10-08T09:22:12.715871"}]})BODY");
}

using namespace FlockMatchmakingTestHelpers;

#define FLOCK_MATCHMAKING_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Matchmaking." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// ---- a search alone ----

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingAloneTest, "FindMatchAloneIsCheckedEveryFewSecondsAndHandsBackTheMatchedSessionHeld")
bool FFlockMatchmakingAloneTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.Server->QueuedBy(FServer::DuelId, PlayerB);
	TestEqual(TEXT("a game checks every 3 s"), FFlockMatchmaking::DefaultCheckIntervalSeconds, 3.0);
	// The shortest wait first, then the longest: a check every 2.25 to 3.75 s.
	*F.Roll = 0.0;
	const FSearchAnswerRef Found = F.Find(FServer::Duel);
	*F.Roll = 1.0;
	TestEqual(TEXT("the search was made"), F.Server->Count(ERoute::Create), 1);
	F.Advance(2.2);
	TestEqual(TEXT("no check before 2.25 s"), F.Server->Count(ERoute::ReadTicket), 0);
	F.Advance(0.1);
	TestEqual(TEXT("the first check, the shortest wait from the start"), F.Server->Count(ERoute::ReadTicket), 1);
	F.Advance(3.7);
	TestEqual(TEXT("none before the longest wait"), F.Server->Count(ERoute::ReadTicket), 1);
	F.Advance(0.1);
	TestEqual(TEXT("the next at 3.75 s"), F.Server->Count(ERoute::ReadTicket), 2);
	TestFalse(TEXT("still searching"), Found->bAnswered);

	F.Server->FormMatches();
	F.Advance(3.8);
	if (!TestTrue(TEXT("matched"), Found->Succeeded()))
	{
		return false;
	}
	const FFlockMatchmakingResult& Result = Found->Value();
	TestTrue(TEXT("a match"), Result.IsMatched());
	TestEqualSensitive(TEXT("its outcome"), Result.Outcome, FString(FlockMatchmakingOutcomes::Matched));
	TestEqualSensitive(TEXT("the longest waiting first"), Joined(Result.PlayerIds), Joined({ PlayerB, PlayerA }));
	if (!TestNotNull(TEXT("the match's session"), Result.Session.Get()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the longest waiting hosts"), Result.Session->GetHostPlayerId(), PlayerB);
	TestTrue(TEXT("held like any other session"), Result.Session == F.HeldSession());
	const TSharedRef<UFlockMultiplayerSession*> Mine = MakeShared<UFlockMultiplayerSession*>(nullptr);
	F.Provider->GetMySession([Mine](TFlockResult<UFlockMultiplayerSession*> Got) { *Mine = Got.bSuccess ? Got.Value : nullptr; });
	TestTrue(TEXT("and it is the player's session"), *Mine == Result.Session);
	TestEqual(TEXT("answered once"), Found->Answers, 1);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingBodyTest, "FindMatchSendsTheQueueItsNameNamesItsAttributesAndNoParty")
bool FFlockMatchmakingBodyTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	FFlockMatchmakingOptions Options;
	Options.Attributes.Set(TEXT("mode"), TEXT("ranked"));
	const FSearchAnswerRef Found = F.Find(FServer::Trio, Options);
	if (!TestEqual(TEXT("the search was made"), F.Server->Count(ERoute::Create), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Body = BodyOf(FlockTestAt(F.Server->RequestsTo(ERoute::Create), 0));
	if (!TestTrue(TEXT("a body"), Body.IsValid()))
	{
		return false;
	}
	FString QueueId;
	Body->TryGetStringField(TEXT("queue_id"), QueueId);
	TestTrue(TEXT("queue_id spelled as the server reads it"), FlockTestSpelling::HasMemberSpelled(Body, TEXT("queue_id")));
	TestEqualSensitive(TEXT("the queue's id, found by its name"), QueueId, FString(FServer::TrioId));
	const TSharedPtr<FJsonObject>* Attributes = nullptr;
	FString Mode;
	TestTrue(TEXT("attributes sent"), FlockTestSpelling::HasMemberSpelled(Body, TEXT("attributes"))
		&& Body->TryGetObjectField(TEXT("attributes"), Attributes) && Attributes != nullptr && (*Attributes)->TryGetStringField(TEXT("mode"), Mode));
	TestEqualSensitive(TEXT("as given"), Mode, FString(TEXT("ranked")));
	TestFalse(TEXT("a search alone names no party"), Body->HasField(TEXT("party_id")));
	Found->Handle.Cancel();
	TestTrue(TEXT("cancelled"), Found->WasCancelled());

	const FSearchAnswerRef Plain = F.Find(FServer::Duel);
	TestEqual(TEXT("the second search was made"), F.Server->Count(ERoute::Create), 2);
	const TSharedPtr<FJsonObject> PlainBody = BodyOf(FlockTestAt(F.Server->RequestsTo(ERoute::Create), 1));
	TestTrue(TEXT("no attributes given, none sent"), PlainBody.IsValid() && !PlainBody->HasField(TEXT("attributes")));
	Plain->Handle.Cancel();
	TestTrue(TEXT("cancelled too"), Plain->WasCancelled());
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingExpiredTest, "ASearchNobodyFillsEndsAsExpiredNotAFailure")
bool FFlockMatchmakingExpiredTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	TestEqual(TEXT("the search was made"), F.Server->Count(ERoute::Create), 1);
	F.Server->ExpiresAll();
	F.Advance(3.0);
	if (!TestTrue(TEXT("not a failure"), Found->Succeeded()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("expired"), Found->Value().Outcome, FString(FlockMatchmakingOutcomes::Expired));
	TestFalse(TEXT("no match"), Found->Value().IsMatched());
	TestNull(TEXT("no session"), Found->Value().Session.Get());
	TestEqual(TEXT("no players"), Found->Value().PlayerIds.Num(), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingStoppedElsewhereTest, "ASearchStoppedByAnotherGameEndsAsCancelled")
bool FFlockMatchmakingStoppedElsewhereTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	const TSharedPtr<FServer::FTicket> Ticket = F.TicketOf(PlayerA);
	if (!TestTrue(TEXT("the search was made"), Ticket.IsValid()))
	{
		return false;
	}
	F.Server->CancelledElsewhere(Ticket.ToSharedRef());
	F.Advance(3.0);
	TestTrue(TEXT("an outcome, not a failure"), Found->Succeeded());
	TestEqualSensitive(TEXT("cancelled"), Found->Value().Outcome, FString(FlockMatchmakingOutcomes::Cancelled));
	TestEqual(TEXT("this game sent no cancel"), F.Server->Count(ERoute::Cancel), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingCancelTest, "CancellingTheHandleCancelsTheSearchOnTheServerAndChecksNoMore")
bool FFlockMatchmakingCancelTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	F.Advance(3.0);
	TestEqual(TEXT("checked once"), F.Server->Count(ERoute::ReadTicket), 1);
	const TSharedRef<FServer::FHeld> Cancelling = F.Server->HoldNext(ERoute::Cancel);
	Found->Handle.Cancel();
	TestTrue(TEXT("the cancel is on its way"), Cancelling->bArrived && !Found->bAnswered);
	F.Advance(3.0);
	F.Advance(3.0);
	TestEqual(TEXT("no check while the cancel is on its way"), F.Server->Count(ERoute::ReadTicket), 1);
	Cancelling->Release();
	TestTrue(TEXT("heard as a cancellation once the server took it"), Found->WasCancelled());
	TestEqual(TEXT("one cancel"), F.Server->Count(ERoute::Cancel), 1);
	TestEqualSensitive(TEXT("nothing left searching"), F.StatusOf(PlayerA), FString(TEXT("cancelled")));
	const int32 Checks = F.Server->Count(ERoute::ReadTicket);
	F.Advance(3.0);
	F.Advance(3.0);
	F.Advance(3.0);
	TestEqual(TEXT("no check after the cancel"), F.Server->Count(ERoute::ReadTicket), Checks);
	TestEqual(TEXT("answered once"), Found->Answers, 1);
	TestFalse(TEXT("nothing held here"), F.Matchmaking().HasSearchForTesting());
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingCancelMatchedTest, "AMatchLandingWhileTheCancelIsOnItsWayGivesTheSeatUpThenTheCancelIsReported")
bool FFlockMatchmakingCancelMatchedTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.Server->QueuedBy(FServer::DuelId, PlayerB);
	// Shared, not captured by reference: a call still waiting when the test returns early is answered by the fixture's shutdown.
	const TSharedRef<bool> SeatedWhenReported = MakeShared<bool>(true);
	const TSharedRef<TSharedPtr<FFlockFakeSessionServer::FSession>> Matched = MakeShared<TSharedPtr<FFlockFakeSessionServer::FSession>>();
	const FSearchAnswerRef Found = F.Find(FServer::Duel, FFlockMatchmakingOptions(), [SeatedWhenReported, Matched]()
	{
		*SeatedWhenReported = Matched->IsValid() && (*Matched)->IsSeated(PlayerA);
	});
	const TArray<TSharedRef<FServer::FMatch>> Matches = F.Server->FormMatches();
	if (!TestEqual(TEXT("one match, before any check"), Matches.Num(), 1))
	{
		return false;
	}
	*Matched = F.Server->Sessions->ById(Matches[0]->SessionId);
	Found->Handle.Cancel();
	TestTrue(TEXT("the cancel is reported"), Found->WasCancelled());
	TestTrue(TEXT("the match's session"), Matched->IsValid());
	TestFalse(TEXT("the seat the match gave was let go before the cancel was reported"), *SeatedWhenReported);
	TestFalse(TEXT("let go"), Matched->IsValid() && (*Matched)->IsSeated(PlayerA));
	TestTrue(TEXT("the other player keeps theirs"), Matched->IsValid() && (*Matched)->IsSeated(PlayerB));
	TestNull(TEXT("no session held"), F.HeldSession());
	TestEqual(TEXT("a search already ended is no error"), F.Log->Errors.Num(), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingCancelOnItsWayTest, "CancellingWhileTheSearchIsOnItsWayCancelsItOnceItLands")
bool FFlockMatchmakingCancelOnItsWayTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const TSharedRef<FServer::FHeld> Making = F.Server->HoldNext(ERoute::Create);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	TestTrue(TEXT("the search is on its way"), Making->bArrived);
	Found->Handle.Cancel();
	TestTrue(TEXT("given up at once"), Found->WasCancelled());
	TestEqual(TEXT("nothing to cancel yet"), F.Server->Count(ERoute::Cancel), 0);

	Making->Release();
	TestEqual(TEXT("cancelled once it landed"), F.Server->Count(ERoute::Cancel), 1);
	TestEqualSensitive(TEXT("on the server too"), F.StatusOf(PlayerA), FString(TEXT("cancelled")));
	F.Advance(3.0);
	F.Advance(3.0);
	TestEqual(TEXT("never checked"), F.Server->Count(ERoute::ReadTicket), 0);
	TestEqual(TEXT("answered once"), Found->Answers, 1);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingGivenUpMatchedTest, "ASearchGivenUpOnItsWayThatMatchedMeanwhileGivesTheSeatUp")
bool FFlockMatchmakingGivenUpMatchedTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.Server->QueuedBy(FServer::DuelId, PlayerB);
	const TSharedRef<FServer::FHeld> Making = F.Server->HoldNext(ERoute::Create);
	const FSearchAnswerRef Found = F.Find(FServer::Duel);
	TestTrue(TEXT("the search is on its way"), Making->bArrived);
	Found->Handle.Cancel();
	TestTrue(TEXT("given up at once"), Found->WasCancelled());
	const TArray<TSharedRef<FServer::FMatch>> Matches = F.Server->FormMatches();
	if (!TestEqual(TEXT("matched meanwhile"), Matches.Num(), 1))
	{
		return false;
	}
	Making->Release();
	const TSharedPtr<FFlockFakeSessionServer::FSession> Matched = F.Server->Sessions->ById(Matches[0]->SessionId);
	TestTrue(TEXT("the match's session"), Matched.IsValid());
	TestFalse(TEXT("the seat the match gave was let go"), Matched.IsValid() && Matched->IsSeated(PlayerA));
	TestTrue(TEXT("the other player keeps theirs"), Matched.IsValid() && Matched->IsSeated(PlayerB));
	TestEqual(TEXT("answered once"), Found->Answers, 1);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingGivenUpLandsLateTest, "ASearchGivenUpOnItsWayThatLandsAfterTheGamesNextSearchNeverCancelsTheNextOne")
bool FFlockMatchmakingGivenUpLandsLateTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	// The given-up search reaches the server after the next one, so the server refuses it as already queued.
	F.Server->AnswerNext(ERoute::Create, FServer::Refused(409, TEXT("matchmaking.already_queued")));
	const TSharedRef<FServer::FHeld> GivenUpMaking = F.Server->HoldNext(ERoute::Create);
	const FSearchAnswerRef First = F.Find(FServer::Trio);
	TestTrue(TEXT("the first search is on its way"), GivenUpMaking->bArrived);
	First->Handle.Cancel();
	TestTrue(TEXT("given up at once"), First->WasCancelled());

	const FSearchAnswerRef Next = F.Find(FServer::Trio);
	F.Advance(3.0);
	TestEqual(TEXT("the next search is checked"), F.Server->Count(ERoute::ReadTicket), 1);
	GivenUpMaking->Release();
	F.Advance(3.0);
	TestEqual(TEXT("nothing of the game's next search is cancelled"), F.Server->Count(ERoute::Cancel), 0);
	TestEqual(TEXT("and nothing is read in its place"), F.Server->Count(ERoute::Current), 0);
	TestEqualSensitive(TEXT("the next search is queued"), F.StatusOf(PlayerA), FString(TEXT("queued")));
	TestFalse(TEXT("and still runs"), Next->bAnswered);

	F.Server->QueuedBy(FServer::TrioId, PlayerB);
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	TestTrue(TEXT("matched"), Next->Succeeded() && Next->Value().IsMatched());
	TestEqual(TEXT("the given-up call answered once"), First->Answers, 1);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingReplacesOwnTest, "ASearchOfThePlayersOwnFromBeforeIsCancelledAndThisOneTakesItsPlace")
bool FFlockMatchmakingReplacesOwnTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FServer::FTicketRef Earlier = F.Server->QueuedBy(FServer::TrioId, PlayerA)[0];
	F.Server->QueuedBy(FServer::DuelId, PlayerB);
	const FSearchAnswerRef Found = F.Find(FServer::Duel);
	TestEqual(TEXT("made again after the refusal"), F.Server->Count(ERoute::Create), 2);
	TestEqualSensitive(TEXT("the earlier search was cancelled"), Earlier->Status, FString(TEXT("cancelled")));
	const TSharedPtr<FServer::FTicket> Queued = F.Server->QueuedTicketOf(PlayerA);
	TestEqualSensitive(TEXT("this search took its place"), Queued.IsValid() ? Queued->QueueId : FString(), FString(FServer::DuelId));
	F.Server->FormMatches();
	F.Advance(3.0);
	TestTrue(TEXT("matched"), Found->Succeeded() && Found->Value().IsMatched());
	TestEqual(TEXT("the refusal it expected is no error"), F.Log->Errors.Num(), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingAlonePartyQueuedTest, "ASearchAloneWhileThePartysSearchRunsIsRefusedOnItsCodeAndLeavesThePartysSearchAlone")
bool FFlockMatchmakingAlonePartyQueuedTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	const FSearchAnswerRef Found = F.Find(FServer::Duel);
	TestTrue(TEXT("refused"), Found->bAnswered && !Found->Result.bSuccess);
	TestEqual(TEXT("on its code"), Found->Result.Error.ErrorCode, EFlockErrorCode::MatchmakingAlreadyQueued);
	const TSharedPtr<FServer::FTicket> Queued = F.Server->QueuedTicketOf(PlayerA);
	TestTrue(TEXT("the party's search goes on"), Queued.IsValid() && Queued->PartyId == PartyId);
	TestEqual(TEXT("nothing cancelled"), F.Server->Count(ERoute::Cancel), 0);
	TestFalse(TEXT("nothing left running"), F.Matchmaking().HasSearchForTesting());
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingSecondSearchTest, "ASecondSearchWhileOneRunsIsRefusedWithoutARequestAndARefusedOneLeavesNoneRunning")
bool FFlockMatchmakingSecondSearchTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FSearchAnswerRef First = F.Find(FServer::Trio);
	TestEqual(TEXT("the search was made"), F.Server->Count(ERoute::Create), 1);
	const FSearchAnswerRef Second = F.Find(FServer::Duel);
	TestTrue(TEXT("refused"), Second->FailedWith(EFlockErrorType::Validation));
	TestEqual(TEXT("no second search went out"), F.Server->Count(ERoute::Create), 1);
	TestFalse(TEXT("the first still runs"), First->bAnswered);
	First->Handle.Cancel();
	TestTrue(TEXT("cancelled"), First->WasCancelled());

	const FSearchAnswerRef Unknown = F.Find(TEXT("no-such-queue"));
	TestTrue(TEXT("an unknown queue is refused"), Unknown->bAnswered && !Unknown->Result.bSuccess);
	TestEqual(TEXT("on its code"), Unknown->Result.Error.ErrorCode, EFlockErrorCode::MatchmakingQueueNotFound);
	const FSearchAnswerRef After = F.Find(FServer::Duel);
	TestEqual(TEXT("a refused search left none running"), F.Server->Count(ERoute::Create), 2);
	TestFalse(TEXT("and the next one runs"), After->bAnswered);
	After->Handle.Cancel();
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingSignInTest, "SignedOutIsRefusedWithoutARequestAndASignInThatEndsMidSearchEndsItSendingNothingMore")
bool FFlockMatchmakingSignInTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.SignOut();
	const FSearchAnswerRef SignedOut = F.Find(FServer::Duel);
	TestTrue(TEXT("refused"), SignedOut->FailedWith(EFlockErrorType::Auth));
	TestEqual(TEXT("without a request"), F.Server->Requests.Num(), 0);

	F.SignIn(PlayerA);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	F.Advance(3.0);
	TestEqual(TEXT("checked once"), F.Server->Count(ERoute::ReadTicket), 1);
	F.SignIn(PlayerC);
	F.Frame();
	TestTrue(TEXT("ended with the sign-in, as cancelled"), Found->WasCancelled());
	const int32 Checks = F.Server->Count(ERoute::ReadTicket);
	F.Advance(3.0);
	F.Advance(3.0);
	TestEqual(TEXT("no check as the next player"), F.Server->Count(ERoute::ReadTicket), Checks);
	TestEqual(TEXT("nothing sent as the next player"), F.Server->Count(ERoute::Cancel), 0);
	TestFalse(TEXT("nothing held here"), F.Matchmaking().HasSearchForTesting());
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingNoSessionTest, "AMatchWithoutASessionIsMatchedNamesThePlayersAndWarnsOnce")
bool FFlockMatchmakingNoSessionTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.Server->bMakesSessions = false;
	F.Server->QueuedBy(FServer::DuelId, PlayerB);
	const FSearchAnswerRef Found = F.Find(FServer::Duel);
	F.Server->FormMatches();
	F.Advance(3.0);
	TestTrue(TEXT("matched"), Found->Succeeded() && Found->Value().IsMatched());
	TestNull(TEXT("no session"), Found->Value().Session.Get());
	TestEqualSensitive(TEXT("the players"), Joined(Found->Value().PlayerIds), Joined({ PlayerB, PlayerA }));
	TestEqual(TEXT("one warning names the setting"), Containing(F.Log->Warnings, TEXT("made no session")), 1);
	TestTrue(TEXT("the setting is in the Flock dashboard"), FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("Flock dashboard")));
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingRetryAfterSwitchTest, "ARetriedCheckAfterAPlayerSwitchIsNotSentAsTheNewPlayer")
bool FFlockMatchmakingRetryAfterSwitchTest::RunTest(const FString& Parameters)
{
	for (const bool bSwitch : { false, true })
	{
		const FString Case = bSwitch ? TEXT("after a switch") : TEXT("control, no switch");
		FMatchmakingFixture F(/*PartyRefreshSeconds*/ 0.0, /*MaxRetries*/ 1);
		F.SignIn(PlayerA);
		F.Server->AnswerNext(ERoute::ReadTicket, FServer::Refused(429, TEXT("request.rate_limited")));
		const TSharedRef<FServer::FHeld> FirstTry = F.Server->HoldNext(ERoute::ReadTicket);
		const FSearchAnswerRef Found = F.Find(FServer::Trio);
		F.Advance(3.0);
		TestTrue(FString::Printf(TEXT("%s: the first check is on its way"), *Case), FirstTry->bArrived);
		FirstTry->Release();
		if (bSwitch)
		{
			F.SignIn(PlayerC);
		}
		// The retry's wait ends before the next frame could end the search, as after a long frame (a level load).
		FMatchmakingFixture::PumpRetries();
		TestEqual(FString::Printf(TEXT("%s: checks sent"), *Case), F.Server->Count(ERoute::ReadTicket), bSwitch ? 1 : 2);
		for (const FString& Player : F.Server->PlayersOf(ERoute::ReadTicket))
		{
			TestEqualSensitive(FString::Printf(TEXT("%s: every check as the searching player"), *Case), Player, PlayerA);
		}
		F.Frame();
		TestEqual(FString::Printf(TEXT("%s: the search ended with its sign-in"), *Case), Found->WasCancelled(), bSwitch);
		if (!bSwitch)
		{
			Found->Handle.Cancel();
		}
	}
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingGameEndingTest, "TheGameEndingCancelsTheSearchWithoutWaitingAndAnswersTheCall")
bool FFlockMatchmakingGameEndingTest::RunTest(const FString& Parameters)
{
	FSubsystemFixture S;
	FFlockMultiplayerProvider* Provider = S.Provider();
	if (!TestNotNull(TEXT("multiplayer is up"), Provider) || !TestTrue(TEXT("signed in"), S.Test->IsAuthenticated()))
	{
		return false;
	}
	const FSearchAnswerRef Found = MakeShared<FSearchAnswer>();
	Found->Handle = Provider->FindMatch(FServer::Trio, FFlockMatchmakingOptions(), Into(Found));
	TestEqual(TEXT("the search was made"), S.Server->Count(ERoute::Create), 1);
	// The game instance ending is the path a quitting game takes.
	S.Test->Deinitialize();
	TestEqual(TEXT("the cancel went out"), S.Server->Count(ERoute::Cancel), 1);
	const TArray<FServer::FTicketRef> Tickets = S.Server->TicketsOf(PlayerA);
	TestTrue(TEXT("nobody who has gone is matched"), Tickets.Num() == 1 && Tickets[0]->Status == TEXT("cancelled"));
	TestTrue(TEXT("the call is answered, as cancelled"), Found->WasCancelled());
	TestEqual(TEXT("once"), Found->Answers, 1);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingShutdownTest, "FlockShuttingDownEndsTheSearchWithoutAWord")
bool FFlockMatchmakingShutdownTest::RunTest(const FString& Parameters)
{
	{
		FSubsystemFixture S;
		FFlockMultiplayerProvider* Provider = S.Provider();
		if (!TestNotNull(TEXT("multiplayer is up"), Provider))
		{
			return false;
		}
		const FSearchAnswerRef Found = MakeShared<FSearchAnswer>();
		Found->Handle = Provider->FindMatch(FServer::Trio, FFlockMatchmakingOptions(), Into(Found));
		TestEqual(TEXT("the search was made"), S.Server->Count(ERoute::Create), 1);
		S.Test->ShutdownSdk();
		TestTrue(TEXT("the call ends as cancelled"), Found->WasCancelled());
		TestEqual(TEXT("nothing sent while Flock shuts down"), S.Server->Count(ERoute::Cancel), 0);
	}

	// A party's search ends the same way, with no event.
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.PartyOf(PlayerA, { PlayerB });
	UFlockParty* Held = F.MyParty(*this);
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const FSearchAnswerRef Found = F.FindAsParty(Held, FServer::Trio);
	TestTrue(TEXT("the party's search started"), Held != nullptr && Held->IsSearching());
	F.Provider->Shutdown();
	TestTrue(TEXT("the party's call ends as cancelled"), Found->WasCancelled());
	TestEqualSensitive(TEXT("and the party heard only the start"), Joined(Listener->SearchHeard), FString(TEXT("started")));
	TestEqual(TEXT("nothing sent"), F.Server->Count(ERoute::Cancel), 0);
	return true;
}

// ---- a party ----

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingLeaderTest, "TheLeaderQueuesThePartyChecksItsOwnTicketAndHearsTheSearchStartAndEnd")
bool FFlockMatchmakingLeaderTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	F.PartyById(PartyId)->LeaderPlayerId = PlayerA;
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const TSharedRef<FString> HeardWhenTheCallReturned = MakeShared<FString>();
	const TWeakObjectPtr<UFlockPartyTestListener> WeakListener = Listener;
	const FSearchAnswerRef Found = F.FindAsParty(Held, FServer::Trio, [HeardWhenTheCallReturned, WeakListener]()
	{
		*HeardWhenTheCallReturned = WeakListener.IsValid() ? Joined(WeakListener->SearchHeard) : FString();
	});
	F.Advance(3.0);
	TestEqual(TEXT("checked once"), F.Server->Count(ERoute::ReadTicket), 1);
	FString SentPartyId;
	const TSharedPtr<FJsonObject> Body = BodyOf(FlockTestAt(F.Server->RequestsTo(ERoute::Create), 0));
	TestTrue(TEXT("the party's id sent"), Body.IsValid() && Body->TryGetStringField(TEXT("party_id"), SentPartyId));
	TestEqualSensitive(TEXT("this party"), SentPartyId, PartyId);
	TestEqualSensitive(TEXT("heard it start"), Joined(Listener->SearchHeard), FString(TEXT("started")));
	TestTrue(TEXT("searching"), Held->IsSearching());
	const TSharedPtr<FServer::FTicket> Own = F.Server->QueuedTicketOf(PlayerA);
	for (const FFlockHttpRequest& Check : F.Server->RequestsTo(ERoute::ReadTicket))
	{
		TestTrue(TEXT("the leader checks its own ticket, not the first the answer lists"),
			Own.IsValid() && Check.Url.EndsWith(Own->Id, ESearchCase::CaseSensitive));
	}

	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	if (!TestTrue(TEXT("matched"), Found->Succeeded()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the party first, never split"), Joined(Found->Value().PlayerIds), Joined({ PlayerB, PlayerA, PlayerC }));
	TestTrue(TEXT("the session held"), Found->Value().Session != nullptr && Found->Value().Session == F.HeldSession());
	TestEqualSensitive(TEXT("heard start and end"), Joined(Listener->SearchHeard), FString(TEXT("started,ended matched")));
	TestEqualSensitive(TEXT("the party's handlers hear the end before the game's call completes"), *HeardWhenTheCallReturned,
		FString(TEXT("started,ended matched")));
	TestTrue(TEXT("the party's result carries the session too"),
		Listener->SearchesEnded.Num() == 1 && Listener->SearchesEnded[0].Session == Found->Value().Session);
	TestFalse(TEXT("no longer searching"), Held->IsSearching());
	return true;
}

/** The leader's search made and matched before this game reads again: the game holding the party is still told. */
static void TheLeadersEarlyMatchIsTold(FAutomationTestBase& Test, FMatchmakingFixture& F, const FString& PartyId, UFlockParty* Held)
{
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const FSearchAnswerRef Found = F.FindAsParty(Held, FString());
	// Made and matched before this game's next reading, as when the server forms matches right after the leader's search.
	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	Test.TestTrue(TEXT("a match made after the game held the party is never taken for an old one"),
		Found->Succeeded() && Found->Value().IsMatched());
	Test.TestTrue(TEXT("the session held"), Found->Value().Session != nullptr && Found->Value().Session == F.HeldSession());
	// TestTrue, not Test.TestEqualSensitive: below UE 5.5 that check is a macro that works only unqualified, inside a test's own RunTest.
	const FString Heard = Joined(Listener->SearchHeard);
	Test.TestTrue(FString::Printf(TEXT("heard start and end (%s)"), *Heard), Heard.Equals(TEXT("started,ended matched"), ESearchCase::CaseSensitive));
	Test.TestEqual(TEXT("a member's game makes no search"), F.Server->Count(ERoute::Create), 0);
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingEarlyMatchJoinTest, "ALeadersSearchMatchedBeforeTheMembersGameReadsAgainIsStillToldToThatGame")
bool FFlockMatchmakingEarlyMatchJoinTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, {});
	const FPartyCallRef JoinCall = F.Join(F.PartyById(PartyId)->InviteCode);
	if (!TestTrue(TEXT("joined"), JoinCall->Succeeded()))
	{
		return false;
	}
	TheLeadersEarlyMatchIsTold(*this, F, PartyId, JoinCall->Party());
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingEarlyMatchMineTest, "ALeadersSearchMatchedBeforeTheNextReadingAfterGetMyPartyIsStillToldToThatGame")
bool FFlockMatchmakingEarlyMatchMineTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	TheLeadersEarlyMatchIsTold(*this, F, PartyId, Held);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingJoinGivenUpTest, "AJoinTheGameGivesUpDuringItsSearchReadingStillHandsOverTheParty")
bool FFlockMatchmakingJoinGivenUpTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, {});
	const TSharedRef<FServer::FHeld> Reading = F.Server->HoldNext(ERoute::Current);
	const FPartyCallRef JoinCall = F.Join(F.PartyById(PartyId)->InviteCode);
	TestTrue(TEXT("the player's search is being read"), Reading->bArrived);
	TestFalse(TEXT("the join waits for it"), JoinCall->bAnswered);
	JoinCall->Handle.Cancel();
	TestTrue(TEXT("the reading was cancelled"), Reading->bDelivered);
	TestTrue(TEXT("the join landed, so the party is the player's"), JoinCall->Succeeded());
	TestTrue(TEXT("held"), JoinCall->Party() != nullptr && JoinCall->Party() == F.HeldParty());
	Reading->Release();
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingMemberRefreshTest, "AMemberLearnsTheLeadersSearchFromThePartyRefreshAndGetsTheMatch")
bool FFlockMatchmakingMemberRefreshTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	TestEqual(TEXT("reading the party read the player's search"), F.Server->Count(ERoute::Current), 1);

	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Advance(10.0);
	TestEqual(TEXT("the leader's search was seen by the refresh"), Listener->SearchesStarted(), 1);
	TestTrue(TEXT("searching"), Held->IsSearching());
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	if (!TestEqual(TEXT("the match was seen"), Listener->SearchesEnded.Num(), 1))
	{
		return false;
	}
	const FFlockMatchmakingResult& Ended = Listener->SearchesEnded[0];
	TestTrue(TEXT("matched"), Ended.IsMatched());
	TestTrue(TEXT("the match's session is held here too"), Ended.Session != nullptr && Ended.Session == F.HeldSession());
	TestEqual(TEXT("with everyone in it"), Ended.Session != nullptr ? Ended.Session->GetPlayers().Num() : 0, 3);
	TestEqual(TEXT("a member's game makes no search"), F.Server->Count(ERoute::Create), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingMembersHandlerTest, "AMembersChangedHandlerSeesTheSameRefreshsSearchState")
bool FFlockMatchmakingMembersHandlerTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	// The search's own check waits ten minutes, so only the party refresh brings the ticket.
	F.Matchmaking().SetCheckIntervalForTesting(600.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Advance(10.0);
	if (!TestTrue(TEXT("the leader's search was followed"), Held->IsSearching()))
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);

	// C joins, which stops the party's search, and one refresh brings both.
	F.Server->Parties->Joins(PartyId, PlayerC);
	F.Advance(10.0);
	TestEqual(TEXT("the refresh showed C"), Listener->MembersChanged, 1);
	TestEqualSensitive(TEXT("and the search's end"), Joined(Listener->SearchHeard), FString(TEXT("ended party_changed")));
	TestTrue(TEXT("inside the members handler the search already reads as ended"),
		Listener->SearchingSeen.Num() == 1 && !Listener->SearchingSeen[0]);
	TestEqual(TEXT("no check of its own"), F.Server->Count(ERoute::ReadTicket), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingMemberWaitsTest, "AMembersFindMatchWaitsForTheLeadersSearchAndReturnsItsMatch")
bool FFlockMatchmakingMemberWaitsTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	const FSearchAnswerRef Found = F.FindAsParty(Held, FString());
	F.Advance(3.0);
	F.Advance(3.0);
	TestFalse(TEXT("waits for the leader"), Found->bAnswered);
	TestEqual(TEXT("a member's game makes no search"), F.Server->Count(ERoute::Create), 0);
	TestFalse(TEXT("not searching yet"), Held->IsSearching());

	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Advance(3.0);
	TestTrue(TEXT("the leader's search was taken up"), Held->IsSearching());
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	TestTrue(TEXT("matched"), Found->Succeeded() && Found->Value().IsMatched());
	TestTrue(TEXT("the session held"), Found->Value().Session != nullptr && Found->Value().Session == F.HeldSession());

	const FSearchAnswerRef Second = F.FindAsParty(Held, FString());
	const FSearchAnswerRef Third = F.FindAsParty(Held, FString());
	TestTrue(TEXT("a third call is refused"), Third->FailedWith(EFlockErrorType::Validation));
	TestFalse(TEXT("the first call still waits"), Second->bAnswered);
	TestEqual(TEXT("a player with no search yet is no error"), F.Log->Errors.Num(), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingRunningAtFirstReadTest, "ASearchAlreadyRunningWhenThePartyIsFirstReadIsFollowedAndToldStarted")
bool FFlockMatchmakingRunningAtFirstReadTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	F.Advance(10.0);
	TestTrue(TEXT("the running search was followed"), Held->IsSearching());
	TestEqualSensitive(TEXT("told like a new one"), Joined(Listener->SearchHeard), FString(TEXT("started")));
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	TestEqualSensitive(TEXT("its end was told"), Joined(Listener->SearchHeard), FString(TEXT("started,ended matched")));
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingSearchFromBeforeTest, "AMemberIsNotToldOfASearchFromBeforeThePartyWasReadButIsOfTheNextOne")
bool FFlockMatchmakingSearchFromBeforeTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	F.Server->QueuedBy(FServer::DuelId, PlayerB, F.PartyById(PartyId));
	F.Server->FormMatches();
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	F.Advance(10.0);
	F.Advance(10.0);
	F.Advance(10.0);
	TestTrue(TEXT("three refreshes read the player's search"), F.Server->Count(ERoute::Current) >= 3);
	TestEqual(TEXT("a search from before is not this launch's"), Listener->SearchHeard.Num(), 0);

	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Advance(10.0);
	TestEqual(TEXT("the next search is told"), Listener->SearchesStarted(), 1);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingBetweenRefreshesTest, "APartyMatchedBetweenTwoRefreshesIsStillToldAsStartedThenMatched")
bool FFlockMatchmakingBetweenRefreshesTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const int32 Readings = F.Server->Count(ERoute::Current);

	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	TestEqual(TEXT("matched before the next refresh"), F.Server->Count(ERoute::Current), Readings);
	F.Advance(10.0);
	TestEqual(TEXT("one refresh"), F.Server->Count(ERoute::Current), Readings + 1);
	TestEqualSensitive(TEXT("told by the reading that saw the match"), Joined(Listener->SearchHeard), FString(TEXT("started,ended matched")));
	TestTrue(TEXT("with a session"), Listener->SearchesEnded.Num() == 1 && Listener->SearchesEnded[0].Session != nullptr);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingPartyChangedTest, "APartysSearchStoppedByAPlayerJoiningEndsAsPartyChanged")
bool FFlockMatchmakingPartyChangedTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerA, { PlayerB });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const FSearchAnswerRef Found = F.FindAsParty(Held, FServer::Trio);
	TestEqual(TEXT("the search was made"), F.Server->Count(ERoute::Create), 1);
	F.Server->Parties->Joins(PartyId, PlayerC);
	F.Advance(3.0);
	TestTrue(TEXT("an outcome, not a failure"), Found->Succeeded());
	TestEqualSensitive(TEXT("party_changed"), Found->Value().Outcome, FString(FlockMatchmakingOutcomes::PartyChanged));
	TestEqualSensitive(TEXT("the party heard it"), Joined(Listener->SearchHeard), FString(TEXT("started,ended party_changed")));
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingLeftMidSearchTest, "APartyThePlayerLeftMidSearchHearsNothingMoreThoughTheCallGetsHowItEnded")
bool FFlockMatchmakingLeftMidSearchTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const FSearchAnswerRef Found = F.FindAsParty(Held, FString());
	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Advance(3.0);
	TestTrue(TEXT("the leader's search was taken up"), Held->IsSearching());

	const TSharedRef<bool> Left = MakeShared<bool>(false);
	Held->Leave([Left](TFlockResult<UFlockParty*> Answer) { *Left = Answer.bSuccess; });
	TestTrue(TEXT("left"), *Left);
	F.Advance(3.0);
	TestTrue(TEXT("the search ended"), Found->Succeeded());
	TestEqualSensitive(TEXT("the call hears how it ended"), Found->Value().Outcome, FString(FlockMatchmakingOutcomes::PartyChanged));
	TestEqualSensitive(TEXT("a party that is no longer the player's raises nothing more"), Joined(Listener->SearchHeard), FString(TEXT("started")));
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingCheckRefusedTest, "AFollowedSearchWhoseCheckFailedIsFollowedAgainByTheNextRefreshWithoutBeingToldTwice")
bool FFlockMatchmakingCheckRefusedTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	F.Server->AnswerNext(ERoute::ReadTicket, FServer::Refused(400, TEXT("request.validation_failed")));
	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Advance(10.0);
	TestEqual(TEXT("followed"), Listener->SearchesStarted(), 1);
	F.Advance(3.0);
	TestTrue(TEXT("the check was refused"), FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("no longer followed")));
	TestFalse(TEXT("no longer followed"), Held->IsSearching());
	const int32 Checks = F.Server->Count(ERoute::ReadTicket);
	F.Advance(10.0);
	F.Advance(3.0);
	TestTrue(TEXT("followed again"), F.Server->Count(ERoute::ReadTicket) > Checks);
	TestEqual(TEXT("told once"), Listener->SearchesStarted(), 1);
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	TestTrue(TEXT("the match was seen"), Listener->SearchesEnded.Num() == 1 && Listener->SearchesEnded[0].IsMatched());
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingOlderReadingTest, "AReadingSentBeforeASearchBeganChangesNothingWhenItLandsAfter")
bool FFlockMatchmakingOlderReadingTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerA, { PlayerB });
	F.Server->QueuedBy(FServer::DuelId, PlayerA, F.PartyById(PartyId));
	F.Server->FormMatches();
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	F.Advance(10.0);

	const TSharedRef<FServer::FHeld> Older = F.Server->HoldNext(ERoute::Current);
	F.Advance(10.0);
	TestTrue(TEXT("a refresh's reading is on its way"), Older->bArrived);
	const FSearchAnswerRef Found = F.FindAsParty(Held, FServer::Trio);
	TestEqual(TEXT("the search was made"), F.Server->Count(ERoute::Create), 1);
	Found->Handle.Cancel();
	TestTrue(TEXT("cancelled"), Found->WasCancelled());
	Older->Release();
	F.Frame();
	TestEqualSensitive(TEXT("the older reading told nothing"), Joined(Listener->SearchHeard), FString(TEXT("started,ended cancelled")));
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingOtherPlayerTest, "AnotherPlayerOfTheSamePartyStartsFromAReadingOfTheirOwn")
bool FFlockMatchmakingOtherPlayerTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerC, { PlayerA, PlayerB });
	F.Server->QueuedBy(FServer::TrioId, PlayerC, F.PartyById(PartyId));
	F.Server->FormMatches();
	TestTrue(TEXT("A's party"), F.GetMine()->Succeeded());
	TestTrue(TEXT("A's reading marked where the party's searches stood"), F.Server->Count(ERoute::Current) >= 1);

	F.SignIn(PlayerB);
	F.Frame();
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const int32 Readings = F.Server->Count(ERoute::Current);
	F.Advance(10.0);
	F.Advance(10.0);
	F.Advance(10.0);
	TestTrue(TEXT("three of B's refreshes read B's search"), F.Server->Count(ERoute::Current) >= Readings + 3);
	TestEqual(TEXT("B's search from before is not this launch's"), Listener->SearchHeard.Num(), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingSwitchWhileMakingTest, "ASwitchWhileTheSearchIsBeingMadeEndsTheCallAsCancelledNotAsABadAnswer")
bool FFlockMatchmakingSwitchWhileMakingTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const TSharedRef<FServer::FHeld> Making = F.Server->HoldNext(ERoute::Create);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	TestTrue(TEXT("the search is on its way"), Making->bArrived);
	F.SignIn(PlayerC);
	F.Frame();
	TestTrue(TEXT("heard as a cancellation at the frame, with its search still on its way"), Found->WasCancelled());
	const FSearchAnswerRef Next = F.Find(FServer::Trio);
	TestEqual(TEXT("the next player's search is made at once"), F.Server->Count(ERoute::Create), 2);
	TestFalse(TEXT("and runs"), Next->bAnswered);
	Making->Release();
	TestEqual(TEXT("the old search's answer sends nothing as the next player"), F.Server->Count(ERoute::Cancel), 0);
	TestEqual(TEXT("answered once"), Found->Answers, 1);
	TestEqual(TEXT("never checked"), F.Server->Count(ERoute::ReadTicket), 0);
	Next->Handle.Cancel();
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingWhileFinishingTest, "AFindMatchCallWhileThePartysSearchIsFinishingWaitsForItsResult")
bool FFlockMatchmakingWhileFinishingTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	F.Server->QueuedBy(FServer::TrioId, PlayerB, F.PartyById(PartyId));
	F.Advance(10.0);
	TestTrue(TEXT("the leader's search was followed"), Held->IsSearching());

	const TSharedRef<FFlockFakePartyServer::FHeld> Reading = F.Server->Sessions->HoldNext(FFlockFakeSessionServer::ERoute::Read);
	F.Server->QueuedBy(FServer::TrioId, PlayerC);
	F.Server->FormMatches();
	F.Advance(3.0);
	TestTrue(TEXT("the match's session is being read"), Reading->bArrived);
	const FSearchAnswerRef Found = F.FindAsParty(Held, FString());
	TestFalse(TEXT("not refused while the party's search finishes"), Found->bAnswered);
	Reading->Release();
	TestTrue(TEXT("matched"), Found->Succeeded() && Found->Value().IsMatched());
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingRefreshWhileMakingTest, "ARefreshLandingWhileTheLeadersSearchIsBeingMadeFollowsNothingAndTheSearchStartsOnce")
bool FFlockMatchmakingRefreshWhileMakingTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 10.0);
	F.SignIn(PlayerA);
	F.PartyOf(PlayerA, { PlayerB });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Held);
	const TSharedRef<FServer::FHeld> Making = F.Server->HoldNext(ERoute::Create);
	const FSearchAnswerRef Found = F.FindAsParty(Held, FServer::Trio);
	TestTrue(TEXT("the search is on its way"), Making->bArrived);
	const int32 Readings = F.Server->Count(ERoute::Current);
	F.Advance(10.0);
	F.Advance(10.0);
	TestEqual(TEXT("two refreshes read the search on its way"), F.Server->Count(ERoute::Current), Readings + 2);
	TestEqual(TEXT("and followed nothing"), Listener->SearchHeard.Num(), 0);
	Making->Release();
	F.Advance(3.0);
	TestFalse(TEXT("the search's own answer begins it"), Found->bAnswered);
	TestTrue(TEXT("checked"), F.Server->Count(ERoute::ReadTicket) >= 1);
	TestEqual(TEXT("told once"), Listener->SearchesStarted(), 1);
	return true;
}

// ---- found by the review ----

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingWaitPartyEndedTest, "AMembersCallWaitingForTheLeaderEndsWhenThePartyEnds")
bool FFlockMatchmakingWaitPartyEndedTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	const FSearchAnswerRef Found = F.FindAsParty(Held, FString());
	F.Advance(3.0);
	TestFalse(TEXT("waits for the leader"), Found->bAnswered);
	const TSharedRef<bool> Left = MakeShared<bool>(false);
	Held->Leave([Left](TFlockResult<UFlockParty*> Answer) { *Left = Answer.bSuccess; });
	TestTrue(TEXT("left"), *Left);
	F.Advance(3.0);
	TestTrue(TEXT("the wait ends"), Found->FailedWith(EFlockErrorType::Validation));
	TestTrue(TEXT("saying the party ended"), Found->Result.Error.Message.Contains(TEXT("has ended (Left)")));
	const int32 Readings = F.Server->Count(ERoute::Current);
	F.Advance(3.0);
	F.Advance(3.0);
	TestEqual(TEXT("nothing more is read"), F.Server->Count(ERoute::Current), Readings);
	const FSearchAnswerRef Alone = F.Find(FServer::Duel);
	TestEqual(TEXT("and a search alone can start"), F.Server->Count(ERoute::Create), 1);
	TestFalse(TEXT("and runs"), Alone->bAnswered);
	Alone->Handle.Cancel();
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingWaitNowLeaderTest, "AMemberWaitingForTheLeaderWhoComesToLeadIsToldToSearchItself")
bool FFlockMatchmakingWaitNowLeaderTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FString PartyId = F.PartyOf(PlayerB, { PlayerA });
	UFlockParty* Held = F.MyParty(*this);
	if (Held == nullptr)
	{
		return false;
	}
	const FSearchAnswerRef Found = F.FindAsParty(Held, FString());
	// The leader leaves on the server, so A leads; A's game learns it from its next reading of the party.
	F.Server->Parties->Leaves(PartyId, PlayerB);
	TestTrue(TEXT("A's game read the party again"), F.GetMine()->Succeeded() && Held->IsLeader());
	F.Advance(3.0);
	TestTrue(TEXT("the wait ends"), Found->FailedWith(EFlockErrorType::Validation));
	TestTrue(TEXT("telling the game to search as the leader"), Found->Result.Error.Message.Contains(TEXT("leads the party now")));
	const FSearchAnswerRef Own = F.FindAsParty(Held, FServer::Trio);
	TestEqual(TEXT("the new leader's search is made"), F.Server->Count(ERoute::Create), 1);
	TestTrue(TEXT("and runs"), !Own->bAnswered && Held->IsSearching());
	Own->Handle.Cancel();
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingQueueMadeAgainTest, "AQueueMadeAgainUnderTheSameNameIsFoundByItsNewId")
bool FFlockMatchmakingQueueMadeAgainTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FSearchAnswerRef First = F.Find(FServer::Duel);
	First->Handle.Cancel();
	TestEqual(TEXT("the queue list was read once"), F.Server->Count(ERoute::Queues), 1);

	const FString NewId = TEXT("01QUEUEDUELAGAIN0000000000");
	F.Server->RemakesQueue(FServer::Duel, NewId);
	const FSearchAnswerRef Again = F.Find(FServer::Duel);
	TestEqual(TEXT("the list was read again"), F.Server->Count(ERoute::Queues), 2);
	TestEqual(TEXT("the search went once by the old id, then by the new"), F.Server->Count(ERoute::Create), 3);
	FString QueueId;
	const TSharedPtr<FJsonObject> Body = BodyOf(FlockTestAt(F.Server->RequestsTo(ERoute::Create), 2));
	TestTrue(TEXT("by the queue's new id"), Body.IsValid() && Body->TryGetStringField(TEXT("queue_id"), QueueId) && QueueId == NewId);
	TestFalse(TEXT("and runs"), Again->bAnswered);
	Again->Handle.Cancel();

	// Counter: a queue deleted for good is refused on its code once the list is read again, without reading it twice.
	F.Server->RemakesQueue(FServer::Trio, FString());
	const FSearchAnswerRef Gone = F.Find(FServer::Trio);
	TestTrue(TEXT("refused"), Gone->bAnswered && !Gone->Result.bSuccess);
	TestEqual(TEXT("on its code"), Gone->Result.Error.ErrorCode, EFlockErrorCode::MatchmakingQueueNotFound);
	TestEqual(TEXT("one more read of the list"), F.Server->Count(ERoute::Queues), 3);
	TestEqual(TEXT("one more search, by the old id"), F.Server->Count(ERoute::Create), 4);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingQuitWhileMakingTest, "QuittingWhileTheSearchIsBeingMadeCancelsItOnceItLands")
bool FFlockMatchmakingQuitWhileMakingTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const TSharedRef<FServer::FHeld> Making = F.Server->HoldNext(ERoute::Create);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	TestTrue(TEXT("the search is on its way"), Making->bArrived);
	F.Matchmaking().CancelForQuit();
	TestTrue(TEXT("the call ends as cancelled"), Found->WasCancelled());
	Making->Release();
	TestEqual(TEXT("its ticket is cancelled once it lands"), F.Server->Count(ERoute::Cancel), 1);
	TestEqualSensitive(TEXT("so nobody who has gone is matched"), F.StatusOf(PlayerA), FString(TEXT("cancelled")));
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingQuitWhileMatchedTest, "QuittingWhileAMatchLandsGivesItsSeatUp")
bool FFlockMatchmakingQuitWhileMatchedTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	F.Server->QueuedBy(FServer::DuelId, PlayerB);
	const FSearchAnswerRef Found = F.Find(FServer::Duel);
	const TSharedRef<FFlockFakePartyServer::FHeld> Reading = F.Server->Sessions->HoldNext(FFlockFakeSessionServer::ERoute::Read);
	const TArray<TSharedRef<FServer::FMatch>> Matches = F.Server->FormMatches();
	F.Advance(3.0);
	if (!TestTrue(TEXT("the match's session is being read"), Reading->bArrived && Matches.Num() == 1))
	{
		return false;
	}
	F.Matchmaking().CancelForQuit();
	Reading->Release();
	const TSharedPtr<FFlockFakeSessionServer::FSession> Matched = F.Server->Sessions->ById(Matches[0]->SessionId);
	TestFalse(TEXT("the seat the match gave was let go"), Matched.IsValid() && Matched->IsSeated(PlayerA));
	TestTrue(TEXT("the other player keeps theirs"), Matched.IsValid() && Matched->IsSeated(PlayerB));
	TestTrue(TEXT("the call ends as cancelled"), Found->WasCancelled());
	TestEqual(TEXT("nothing to cancel: the ticket was matched"), F.Server->Count(ERoute::Cancel), 0);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingWatchReadOnceTest, "APartyCallDoesNotWaitOutRetriesOfItsSearchReading")
bool FFlockMatchmakingWatchReadOnceTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F(/*PartyRefreshSeconds*/ 0.0, /*MaxRetries*/ 2);
	F.SignIn(PlayerA);
	F.PartyOf(PlayerB, { PlayerA });
	F.Server->AnswerNext(ERoute::Current, FServer::Refused(503, TEXT("server.unavailable")));
	const FPartyCallRef Mine = F.GetMine();
	TestTrue(TEXT("the party is handed over without waiting for a retry"), Mine->Succeeded());
	FMatchmakingFixture::PumpRetries();
	TestEqual(TEXT("the reading was not tried again"), F.Server->Count(ERoute::Current), 1);

	// Control: a check of a search by its id is still tried again.
	const FSearchAnswerRef Found = F.Find(FServer::Duel);
	F.Server->AnswerNext(ERoute::ReadTicket, FServer::Refused(503, TEXT("server.unavailable")));
	F.Advance(3.0);
	FMatchmakingFixture::PumpRetries();
	TestEqual(TEXT("control: the check was tried again"), F.Server->Count(ERoute::ReadTicket), 2);
	Found->Handle.Cancel();
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingMatchedWithoutMatchTest, "AMatchedAnswerWithoutItsMatchIsNotBlamedOnTheSetting")
bool FFlockMatchmakingMatchedWithoutMatchTest::RunTest(const FString& Parameters)
{
	FMatchmakingFixture F;
	F.SignIn(PlayerA);
	const FSearchAnswerRef Found = F.Find(FServer::Trio);
	const TSharedPtr<FServer::FTicket> Ticket = F.TicketOf(PlayerA);
	if (!TestTrue(TEXT("the search was made"), Ticket.IsValid()))
	{
		return false;
	}
	FFlockHttpResponse Matched;
	Matched.Result = EFlockHttpResult::Success;
	Matched.StatusCode = 200;
	Matched.Body = FString::Printf(TEXT("{\"error\":{\"code\":null},\"response\":{\"message\":null,\"code\":null},\"result\":{\"id\":\"%s\",\"status\":\"matched\",\"party_id\":null,\"match\":null}}"),
		*Ticket->Id);
	F.Server->AnswerNext(ERoute::ReadTicket, Matched);
	F.Advance(3.0);
	TestTrue(TEXT("matched"), Found->Succeeded() && Found->Value().IsMatched());
	TestNull(TEXT("no session known"), Found->Value().Session.Get());
	TestTrue(TEXT("the warning says the answer carried no match"), FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("carried no match")));
	TestFalse(TEXT("and does not blame the dashboard setting"), FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("multiplayer settings")));
	return true;
}

// ---- readings, off the server's real answers ----

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingReadMatchedTest, "Readings.AReadOfAMatchedSearchNamesThePlayersInOrderAndTheSession")
bool FFlockMatchmakingReadMatchedTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Object = JsonOf(MatchedRead);
	FFlockMatchmakingTicketReading Ticket;
	FString Error;
	if (!TestTrue(TEXT("read"), Object.IsValid() && FFlockMatchmakingTicketReading::FromWireObject(Object.ToSharedRef(), Ticket, Error)))
	{
		return false;
	}
	TestEqualSensitive(TEXT("its id"), Ticket.Id, FString(TEXT("01M4DD484KEYM8KCGGZZMK7S3S")));
	TestEqualSensitive(TEXT("its player"), Ticket.PlayerId, FString(TEXT("01M46J2SEP5GZ15W8TV9N2Q6MY")));
	TestTrue(TEXT("matched"), Ticket.IsMatched());
	TestTrue(TEXT("a party of null is a search alone"), Ticket.PartyId.IsEmpty() && Ticket.IsForParty(FString()));
	TestTrue(TEXT("no cancel reason"), Ticket.CancelReason.IsEmpty());
	TestTrue(TEXT("the match"), Ticket.bHasMatch);
	TestEqualSensitive(TEXT("its id"), Ticket.Match.Id, FString(TEXT("01M4DD4GAJB0W526MC8ZJ1X3ZQ")));
	TestEqualSensitive(TEXT("the players in the server's order"), Joined(Ticket.Match.PlayerIds),
		FString(TEXT("01M46J2QM0KSQ1X3QNF03RRT6V,01M46J2QXJHZQPWEVG4AJX6MMF,01M46J2SEP5GZ15W8TV9N2Q6MY")));
	TestEqualSensitive(TEXT("the session"), Ticket.Match.SessionId, FString(TEXT("01M4DD4GAPBE2N9FDZVVWJFFYT")));

	// The same match where the game's settings make no session: connection_info answers empty.
	const TSharedPtr<FJsonObject> NoSession = JsonOf(FString(MatchedRead).Replace(
		TEXT("{\"mode\":\"p2p\",\"session_id\":\"01M4DD4GAPBE2N9FDZVVWJFFYT\"}"), TEXT("{}")));
	FFlockMatchmakingTicketReading WithoutSession;
	TestTrue(TEXT("read without a session"), NoSession.IsValid() && FFlockMatchmakingTicketReading::FromWireObject(NoSession.ToSharedRef(), WithoutSession, Error));
	TestTrue(TEXT("matched, with no session"), WithoutSession.bHasMatch && WithoutSession.Match.SessionId.IsEmpty() && WithoutSession.Match.PlayerIds.Num() == 3);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingReadEndedTest, "Readings.AnEndedSearchCarriesHowItEndedAndNoMatch")
bool FFlockMatchmakingReadEndedTest::RunTest(const FString& Parameters)
{
	FString Error;
	FFlockMatchmakingTicketReading Expired;
	const TSharedPtr<FJsonObject> ExpiredObject = JsonOf(ExpiredRead);
	TestTrue(TEXT("an expired read"), ExpiredObject.IsValid() && FFlockMatchmakingTicketReading::FromWireObject(ExpiredObject.ToSharedRef(), Expired, Error));
	TestEqualSensitive(TEXT("expired"), Expired.Status, FString(FlockMatchmakingWireValues::TicketExpired));
	TestFalse(TEXT("a match of null is none"), Expired.bHasMatch);
	TestFalse(TEXT("neither queued nor matched"), Expired.IsQueued() || Expired.IsMatched());

	FFlockMatchmakingTicketReading Cancelled;
	const TSharedPtr<FJsonObject> CancelledObject = JsonOf(CancelAnswer);
	TestTrue(TEXT("a cancel's answer"), CancelledObject.IsValid() && FFlockMatchmakingTicketReading::FromWireObject(CancelledObject.ToSharedRef(), Cancelled, Error));
	TestEqualSensitive(TEXT("cancelled"), Cancelled.Status, FString(FlockMatchmakingWireValues::TicketCancelled));
	TestEqualSensitive(TEXT("by the player"), Cancelled.CancelReason, FString(TEXT("player")));
	TestFalse(TEXT("a cancel's answer carries no match at all"), Cancelled.bHasMatch);
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingReadPartyMadeTest, "Readings.TheAnswerToAPartysSearchListsEveryMembersTicket")
bool FFlockMatchmakingReadPartyMadeTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Envelope = JsonOf(PartyTicketsMade);
	FFlockMatchmakingTicketsMade Made;
	FString Error;
	if (!TestTrue(TEXT("read"), Envelope.IsValid() && FFlockMatchmakingTicketsMade::FromWireObject(Envelope.ToSharedRef(), Made, Error)))
	{
		return false;
	}
	TestEqual(TEXT("one ticket a member"), Made.Tickets.Num(), 2);
	const FFlockMatchmakingTicketReading* Second = Made.FindTicketOf(TEXT("01M46J2QXJHZQPWEVG4AJX6MMF"));
	TestTrue(TEXT("the player's own, wherever the server lists it"), Second != nullptr && Second->Id == TEXT("01M4DD403C8AVYGYQKZ87Y7BKQ"));
	TestTrue(TEXT("the party's"), Second != nullptr && Second->IsForParty(TEXT("01M4DD3Z5XA9NQJ74SBD1BX698")) && Second->IsQueued());
	TestNull(TEXT("found by the id spelled exactly"), Made.FindTicketOf(TEXT("01m46j2qxjhzqpwevg4ajx6mmf")));
	TestNull(TEXT("a player not in it has none"), Made.FindTicketOf(TEXT("someone-else")));
	return true;
}

FLOCK_MATCHMAKING_TEST(FFlockMatchmakingReadRefusedTest, "Readings.AnAnswerThatNamesNoSearchFailsTheRead")
bool FFlockMatchmakingReadRefusedTest::RunTest(const FString& Parameters)
{
	FString Error;
	FFlockMatchmakingTicketReading Ticket;
	const TSharedPtr<FJsonObject> NoId = JsonOf(TEXT("{\"id\":null,\"status\":\"queued\"}"));
	TestFalse(TEXT("no id"), FFlockMatchmakingTicketReading::FromWireObject(NoId.ToSharedRef(), Ticket, Error));
	TestTrue(TEXT("says why"), Error.Contains(TEXT("names no search")));
	const TSharedPtr<FJsonObject> NoStatus = JsonOf(TEXT("{\"id\":\"t-1\"}"));
	TestFalse(TEXT("no status"), FFlockMatchmakingTicketReading::FromWireObject(NoStatus.ToSharedRef(), Ticket, Error));

	FFlockMatchmakingTicketsMade Made;
	const TCHAR* Refused[] = {
		TEXT("{\"result\":{\"id\":\"t-1\",\"status\":\"queued\"}}"),
		TEXT("{\"result\":[]}"),
		TEXT("{\"result\":[\"t-1\"]}"),
		TEXT("{\"result\":[{\"id\":\"t-1\"}]}"),
		TEXT("{\"error\":{\"code\":null}}"),
	};
	for (const TCHAR* Body : Refused)
	{
		const TSharedPtr<FJsonObject> Object = JsonOf(Body);
		TestFalse(FString::Printf(TEXT("refused: %s"), Body), Object.IsValid() && FFlockMatchmakingTicketsMade::FromWireObject(Object.ToSharedRef(), Made, Error));
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
