// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Containers/Ticker.h"
#include "Engine/DebugCameraController.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockSubsystem.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "Multiplayer/FlockJoinChecks.h"
#include "Misc/ScopeExit.h"
#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Tests/Support/FlockConnectingFixture.h"
#include "Tests/Support/FlockTestSpelling.h"

namespace FlockJoinVerificationTestHelpers
{
	using namespace FlockConnectingTestHelpers;

	/** What the engine was handed for one joining player: empty lets them in. */
	struct FJoinAnswer
	{
		bool bAnswered = false;
		int32 Times = 0;
		FString Refusal;

		bool LetIn() const { return bAnswered && Refusal.IsEmpty(); }

		/** The reason Flock's check gave, or a marker when it gave none (let in, unanswered, or words of another's). */
		FString RefusedFor() const
		{
			FString Reason;
			return bAnswered && FlockHostRefusals::TryRead(Refusal, Reason) ? Reason : FString(TEXT("<not refused by Flock's check>"));
		}
	};

	struct FJoining
	{
		uint64 Connection = 0;
		TSharedRef<FJoinAnswer> Answer = MakeShared<FJoinAnswer>();
	};

	const FString JoinersAddress = TEXT("198.51.100.20");

	/** The options a joining player's travel URL carries to the host, as the engine hands them over (measured: the token first). */
	inline FString WithToken(const FString& Token)
	{
		return FString::Printf(TEXT("?FlockJoinToken=%s?Name=Ann?SplitscreenCount=1"), *Token);
	}

	/** Stand-in player controllers the check never uses as players: the class defaults of two controller classes. */
	inline const APlayerController* HostsOwnController() { return GetDefault<APlayerController>(); }
	inline const APlayerController* JoinedController() { return GetDefault<ADebugCameraController>(); }

	struct FJoinFixture : FConnectingFixture
	{
		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
		/** Every game's-check call, in order. */
		TArray<FFlockJoiningPlayer> GameAsked;
		/** Every engine-checks call. */
		int32 EngineAsked = 0;

		FFlockJoinChecksOnHost& Checks() { return Provider->GetJoinChecks(); }

		/** This game (A) hosts a session through Travel To Session: its level listening on 7777 and its direct connection published. */
		UFlockMultiplayerSession* HostingAsA(FAutomationTestBase& Test)
		{
			SignIn(PlayerA);
			UFlockMultiplayerSession* Held = Hosted(Test);
			if (Held == nullptr)
			{
				return nullptr;
			}
			Served = Server->ById(Held->GetId()).ToSharedRef();
			Game->OwnPlayers.Add(HostsOwnController());
			const TSharedRef<FTravelAnswer> Hosting = Travel(Held);
			Test.TestTrue(TEXT("precondition: checking from before the level listens"), Checks().IsHostingForTesting());
			Game->Opens(7777);
			FramesUntil([&]() { return Hosting->bAnswered; });
			if (!Test.TestTrue(TEXT("precondition: hosting"), Hosting->Succeeded()
				&& Hosting->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::StartedAsHost, ESearchCase::CaseSensitive)))
			{
				return nullptr;
			}
			return Held;
		}

		/** A player seated in the hosted session, and a token the server minted for them. */
		FString SeatedWithToken(const FString& PlayerId)
		{
			Server->Joins(Served, PlayerId);
			return Server->MintsJoinToken(Served, PlayerId);
		}

		/**
		 * One player's connection reaches the host and the game mode's PreLoginAsync hands it to Flock's check, as the engine asks it.
		 * AskedOptions is what the game mode passes, the connection's own options unless a test says otherwise.
		 */
		FJoining Joins(const FString& Options, FFlockApproveJoiningPlayer GamesCheck = nullptr, TFunction<FString()> EngineChecks = nullptr,
			const TOptional<FString>& AskedOptions = TOptional<FString>())
		{
			FJoining Joining;
			Joining.Connection = Game->Connects(Options, JoinersAddress);
			FFlockJoiningRequest Request;
			Request.Options = AskedOptions.Get(Options);
			Request.Address = JoinersAddress;
			Request.RunEngineChecks = [this, EngineChecks]()
			{
				++EngineAsked;
				return EngineChecks ? EngineChecks() : FString();
			};
			const TSharedRef<FJoinAnswer> Answer = Joining.Answer;
			Request.Answer = [Answer](const FString& Refusal)
			{
				Answer->bAnswered = true;
				++Answer->Times;
				Answer->Refusal = Refusal;
			};
			Request.IsAskerAlive = []() { return true; };
			Request.ApproveJoiningPlayer = GamesCheck ? MoveTemp(GamesCheck) : FFlockApproveJoiningPlayer([this](const FFlockJoiningPlayer& Player, FString&)
			{
				GameAsked.Add(Player);
				return true;
			});
			Checks().Check(MoveTemp(Request));
			return Joining;
		}

		/** The connection plays through JoinedController from now on, as the engine hands a player controller to one let in. */
		void PlaysThrough(uint64 Connection) { Game->Controllers.Add(JoinedController(), Connection); }

		/** The token each verify request carried, in order. */
		TArray<FString> TokensVerified() const
		{
			TArray<FString> Tokens;
			for (const FFlockHttpRequest& Request : Server->RequestsTo(ERoute::Verify))
			{
				Tokens.Add(Field(BodyOf(Request), TEXT("token")));
			}
			return Tokens;
		}

		int32 ErrorsContaining(const FString& Needle) const
		{
			return Log->Errors.FilterByPredicate([&](const FString& Line) { return Line.Contains(Needle); }).Num();
		}
	};
}

using namespace FlockJoinVerificationTestHelpers;

#define FLOCK_JOIN_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.JoinVerification." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// ---- the host lets in only players Flock vouches for ----

FLOCK_JOIN_TEST(FFlockJoinLetInTest, "Host.APlayerWithTheirToken_IsLetIn_AndKnownByTheirConnection")
bool FFlockJoinLetInTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining Joining = F.Joins(WithToken(Token));
	TestTrue(TEXT("let in"), Joining.Answer->LetIn());
	TestEqual(TEXT("answered once"), Joining.Answer->Times, 1);
	TestEqual(TEXT("one check with Flock"), F.Server->Count(ERoute::Verify), 1);
	TestEqualSensitive(TEXT("the token the player sent was checked with Flock"), FString::Join(F.TokensVerified(), TEXT(",")), Token);
	F.PlaysThrough(Joining.Connection);
	TestEqualSensitive(TEXT("known by their connection"), Session->PlayerForConnection(JoinedController()), PlayerB);
	TestEqualSensitive(TEXT("the host's own controller is its own player"), Session->PlayerForConnection(HostsOwnController()), PlayerA);
	TestTrue(TEXT("no controller is nobody"), Session->PlayerForConnection(nullptr).IsEmpty());
	F.Game->Controllers.Add(JoinedController(), 999);
	TestTrue(TEXT("a connection that is not here is nobody"), Session->PlayerForConnection(JoinedController()).IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinNoTokenTest, "Host.APlayerWithNoToken_IsRefused_WithoutAskingFlock")
bool FFlockJoinNoTokenTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const FJoining None = F.Joins(TEXT("?Name=Ann?SplitscreenCount=1"));
	TestEqualSensitive(TEXT("no token"), None.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::NoJoinToken));
	const FJoining Empty = F.Joins(TEXT("?FlockJoinToken=?Name=Ann"));
	TestEqualSensitive(TEXT("a token of no length"), Empty.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::NoJoinToken));
	TestEqual(TEXT("Flock was not asked"), F.Server->Count(ERoute::Verify), 0);
	TestEqual(TEXT("neither the engine nor the game was asked"), F.EngineAsked + F.GameAsked.Num(), 0);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinBadTokensTest, "Host.AMadeUpToken_AnotherSessionsToken_AndAnExpiredOne_AreRefusedByFlock")
bool FFlockJoinBadTokensTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const FFlockFakeSessionServer::FSessionRef Another = F.Server->HostedBy(PlayerC);
	const FString AnotherSessions = F.Server->MintsJoinToken(Another, PlayerC);

	const FJoining MadeUp = F.Joins(WithToken(TEXT("made-up")));
	TestEqualSensitive(TEXT("made up"), MadeUp.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::JoinTokenInvalid));
	const FJoining Elsewhere = F.Joins(WithToken(AnotherSessions));
	TestEqualSensitive(TEXT("another session's"), Elsewhere.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::JoinTokenInvalid));
	const FString Expiring = F.SeatedWithToken(PlayerB);
	F.Server->ExpireJoinTokens();
	const FJoining Late = F.Joins(WithToken(Expiring));
	TestEqualSensitive(TEXT("expired"), Late.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::JoinTokenInvalid));

	TestEqualSensitive(TEXT("each refused on Flock's answer to it"), FString::Join(F.TokensVerified(), TEXT(",")),
		FString::Join(TArray<FString>{ TEXT("made-up"), AnotherSessions, Expiring }, TEXT(",")));
	TestEqual(TEXT("the game was never asked"), F.GameAsked.Num(), 0);
	// Flock's verdict is an answer the check turns into a refusal: a stranger's made-up tokens do not fill the host's log with errors.
	TestEqual(TEXT("no error logged for Flock's verdict"), F.Log->Errors.Num(), 0);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinSamePlayerTwiceTest, "Host.TheSamePlayerTwice_TheSecondIsRefused_AndTheFirstStays")
bool FFlockJoinSamePlayerTwiceTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining First = F.Joins(WithToken(Token));
	TestTrue(TEXT("the first let in"), First.Answer->LetIn());
	F.PlaysThrough(First.Connection);

	// The same token checks out again within its 60 s, so only the host can turn the second connection away.
	const FJoining Second = F.Joins(WithToken(Token));
	TestEqualSensitive(TEXT("the second"), Second.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::AlreadyConnected));
	TestEqual(TEXT("Flock vouched for the token both times"), F.Server->Count(ERoute::Verify), 2);
	TestEqualSensitive(TEXT("the first keeps its place"), Session->PlayerForConnection(JoinedController()), PlayerB);
	TestEqual(TEXT("the game was asked about the first only"), F.GameAsked.Num(), 1);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinPlayerWhoLeftTest, "Host.APlayerWhoLeft_CanConnectAgain")
bool FFlockJoinPlayerWhoLeftTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining First = F.Joins(WithToken(Token));
	F.PlaysThrough(First.Connection);
	F.Game->Leaves(First.Connection);
	TestTrue(TEXT("a connection that left is nobody's"), Session->PlayerForConnection(JoinedController()).IsEmpty());

	const FJoining Again = F.Joins(WithToken(Token));
	TestTrue(TEXT("let in again"), Again.Answer->LetIn());
	F.PlaysThrough(Again.Connection);
	TestEqualSensitive(TEXT("known by the new connection"), Session->PlayerForConnection(JoinedController()), PlayerB);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinSessionEndedTest, "Host.APlayerJoiningASessionThatEnded_IsRefused_SessionEnded")
bool FFlockJoinSessionEndedTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	F.Server->Ends(F.Served);
	const FJoining Player = F.Joins(WithToken(Token));
	TestEqualSensitive(TEXT("refused"), Player.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::SessionEnded));
	TestTrue(TEXT("the host learned why from Flock"), Session->HasEnded());

	const int32 Asked = F.Server->Count(ERoute::Verify);
	const FJoining After = F.Joins(WithToken(Token));
	TestEqualSensitive(TEXT("a session known to have ended"), After.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::SessionEnded));
	TestEqual(TEXT("without asking Flock"), F.Server->Count(ERoute::Verify), Asked);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinNotHostTest, "Host.APlayerReachingADeviceNoLongerHosting_IsRefused_NotHost")
bool FFlockJoinNotHostTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	F.SeatedWithToken(PlayerB);
	const FString Token = F.SeatedWithToken(PlayerC);
	F.Server->MakesHost(F.Served, PlayerB);
	const FJoining Player = F.Joins(WithToken(Token));
	TestEqualSensitive(TEXT("refused"), Player.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::NotHost));
	return true;
}

// ---- when Flock cannot answer, nobody unchecked gets in ----

FLOCK_JOIN_TEST(FFlockJoinUnreachableTest, "Host.FlockAnsweringSomethingElse_RefusesThePlayer_AsUnreachable")
bool FFlockJoinUnreachableTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	F.Server->AnswerNext(ERoute::Verify, FFlockFakeSessionServer::Refused(422, TEXT("request.validation_failed")));
	const FJoining Player = F.Joins(WithToken(Token));
	TestEqualSensitive(TEXT("refused"), Player.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::FlockUnreachable));
	TestEqual(TEXT("said why"), F.WarningsContaining(TEXT("Flock could not check their join token")), 1);

	// Over the host's checks a minute (measured: 429 request.rate_limited, Retry-After): refused the same way, as the owner chose.
	F.Server->AnswerNext(ERoute::Verify, FFlockFakeSessionServer::Refused(429, TEXT("request.rate_limited")));
	const FJoining Limited = F.Joins(WithToken(Token));
	TestEqualSensitive(TEXT("refused past the limit"), Limited.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::FlockUnreachable));
	TestEqual(TEXT("the game was never asked"), F.GameAsked.Num(), 0);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinNeverAnswersTest, "Host.AFlockCheckThatNeverAnswers_IsRefusedAfterEightSeconds_AndALateYesLetsNobodyIn")
bool FFlockJoinNeverAnswersTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Checking = F.Server->HoldNext(ERoute::Verify);
	const FJoining Player = F.Joins(WithToken(Token));
	TestTrue(TEXT("the check was sent"), Checking->bArrived);
	F.Advance(7.9);
	TestFalse(TEXT("given its time first"), Player.Answer->bAnswered);
	F.Advance(0.2);
	TestEqualSensitive(TEXT("refused once its 8 s ran out"), Player.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::FlockUnreachable));
	TestEqual(TEXT("said why"), F.WarningsContaining(TEXT("gave no answer to the check of their join token within 8 s")), 1);
	TestEqual(TEXT("no check waits"), F.Checks().GetChecksWaitingForTesting(), 0);
	TestTrue(TEXT("the request given up, not only its answer ignored"), Checking->bDelivered);

	Checking->Release();
	F.Frame();
	TestEqual(TEXT("a late yes lets nobody in: answered once"), Player.Answer->Times, 1);
	F.PlaysThrough(Player.Connection);
	TestTrue(TEXT("and remembers nobody"), Session->PlayerForConnection(JoinedController()).IsEmpty());
	TestEqual(TEXT("the game was never asked"), F.GameAsked.Num(), 0);

	// A player who left before the 8 s ran out is answered, but nobody was refused for Flock's silence.
	const TSharedRef<FFlockFakePartyServer::FHeld> Again = F.Server->HoldNext(ERoute::Verify);
	const FJoining Leaving = F.Joins(WithToken(Token));
	F.Game->Leaves(Leaving.Connection);
	F.Advance(8.1);
	TestTrue(TEXT("answered"), Leaving.Answer->bAnswered && !Leaving.Answer->LetIn());
	TestEqual(TEXT("not warned about"), F.WarningsContaining(TEXT("gave no answer to the check of their join token")), 1);
	Again->Release();
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinTenChecksTest, "Host.TenChecksWaitingOnFlock_AreEachAnsweredOnFlocksAnswer")
bool FFlockJoinTenChecksTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	TArray<TSharedRef<FFlockFakePartyServer::FHeld>> Checks;
	TArray<FJoining> Joinings;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		const FString Token = F.SeatedWithToken(FString::Printf(TEXT("player-%d"), Index));
		Checks.Add(F.Server->HoldNext(ERoute::Verify));
		Joinings.Add(F.Joins(WithToken(Token)));
	}
	TestEqual(TEXT("ten checks waiting on Flock"), F.Checks().GetChecksWaitingForTesting(), 10);
	TestTrue(TEXT("none answered yet"), !Joinings.ContainsByPredicate([](const FJoining& Each) { return Each.Answer->bAnswered; }));
	for (const TSharedRef<FFlockFakePartyServer::FHeld>& Check : Checks)
	{
		Check->Release();
	}
	TestTrue(TEXT("each let in on Flock's answer"), !Joinings.ContainsByPredicate([](const FJoining& Each) { return !Each.Answer->LetIn(); }));
	F.Game->Controllers.Add(JoinedController(), Joinings[7].Connection);
	TestEqualSensitive(TEXT("each known by their connection"), Session->PlayerForConnection(JoinedController()), FString(TEXT("player-7")));
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinRefusedHoldsNoPlaceTest, "Host.AConnectionTheGameRefused_HoldsNoPlace")
bool FFlockJoinRefusedHoldsNoPlaceTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	int32 Calls = 0;
	const FFlockApproveJoiningPlayer RefusesTheFirst = [&Calls](const FFlockJoiningPlayer&, FString&) { return ++Calls != 1; };
	// Both still on the host: the engine drops a refused connection only after it is answered.
	const FJoining Refused = F.Joins(WithToken(Token), RefusesTheFirst);
	TestEqualSensitive(TEXT("the game refused the first"), Refused.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::RefusedByTheGame));
	F.PlaysThrough(Refused.Connection);
	TestTrue(TEXT("a refused connection is nobody's"), Session->PlayerForConnection(JoinedController()).IsEmpty());

	const FJoining Next = F.Joins(WithToken(Token), RefusesTheFirst);
	TestTrue(TEXT("the second let in"), Next.Answer->LetIn());
	F.PlaysThrough(Next.Connection);
	TestEqualSensitive(TEXT("known"), Session->PlayerForConnection(JoinedController()), PlayerB);
	return true;
}

// ---- the engine's own checks and the game's own, after Flock's ----

FLOCK_JOIN_TEST(FFlockJoinGamesCheckTest, "Host.TheGamesOwnCheck_RunsAfterFlocks_WithThePlayerAndTheGamesOwnOptions")
bool FFlockJoinGamesCheckTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const FJoining MadeUp = F.Joins(WithToken(TEXT("made-up")));
	TestEqualSensitive(TEXT("made up"), MadeUp.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::JoinTokenInvalid));
	TestEqual(TEXT("never asked about a player Flock refused"), F.GameAsked.Num(), 0);
	TestEqual(TEXT("nor the engine"), F.EngineAsked, 0);

	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining Player = F.Joins(FString::Printf(TEXT("?Name=Ann?FlockJoinToken=%s?SplitscreenCount=1?Team=Red"), *Token));
	TestTrue(TEXT("let in"), Player.Answer->LetIn());
	if (TestEqual(TEXT("asked once"), F.GameAsked.Num(), 1))
	{
		TestEqualSensitive(TEXT("Flock's verified player"), F.GameAsked[0].PlayerId, PlayerB);
		TestEqualSensitive(TEXT("the game's own options, without the token"), F.GameAsked[0].Options, FString(TEXT("?Name=Ann?SplitscreenCount=1?Team=Red")));
		TestEqualSensitive(TEXT("the address the engine gave"), F.GameAsked[0].Address, JoinersAddress);
	}
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinGamesRefusalTest, "Host.TheGamesOwnRefusal_StandsWithItsReason_AndKeepsNoPlace")
bool FFlockJoinGamesRefusalTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	int32 Calls = 0;
	const FFlockApproveJoiningPlayer GamesCheck = [&Calls](const FFlockJoiningPlayer&, FString& OutReason)
	{
		++Calls;
		if (Calls == 1)
		{
			OutReason = TEXT("banned");
			return false;
		}
		return Calls != 2;
	};
	const FJoining Banned = F.Joins(WithToken(Token), GamesCheck);
	TestEqualSensitive(TEXT("the game's own reason"), Banned.Answer->RefusedFor(), FString(TEXT("banned")));
	const FJoining NoReason = F.Joins(WithToken(Token), GamesCheck);
	TestEqualSensitive(TEXT("no reason given"), NoReason.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::RefusedByTheGame));
	const FJoining Allowed = F.Joins(WithToken(Token), GamesCheck);
	TestTrue(TEXT("let in"), Allowed.Answer->LetIn());
	F.PlaysThrough(Allowed.Connection);
	TestEqualSensitive(TEXT("a refusal kept no place for the player"), Session->PlayerForConnection(JoinedController()), PlayerB);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinEnginesChecksTest, "Host.TheEnginesOwnChecks_RunAfterFlocks_BeforeTheGames_AndTheirRefusalStands")
bool FFlockJoinEnginesChecksTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	TArray<FString> Order;
	const FJoining Full = F.Joins(WithToken(Token),
		[&Order](const FFlockJoiningPlayer&, FString&) { Order.Add(TEXT("game")); return true; },
		[&Order, &F]() { Order.Add(F.Server->Count(ERoute::Verify) == 1 ? TEXT("engine after Flock") : TEXT("engine before Flock")); return FString(TEXT("Server full.")); });
	TestEqualSensitive(TEXT("the engine's own words"), Full.Answer->RefusedFor(), FString(TEXT("Server full.")));
	TestEqualSensitive(TEXT("the engine after Flock, and the game not asked"), FString::Join(Order, TEXT(",")), FString(TEXT("engine after Flock")));

	Order.Reset();
	const FJoining Room = F.Joins(WithToken(Token),
		[&Order](const FFlockJoiningPlayer&, FString&) { Order.Add(TEXT("game")); return true; },
		[&Order]() { Order.Add(TEXT("engine")); return FString(); });
	TestTrue(TEXT("let in"), Room.Answer->LetIn());
	TestEqualSensitive(TEXT("the engine's checks before the game's"), FString::Join(Order, TEXT(",")), FString(TEXT("engine,game")));
	return true;
}

// ---- a game mode that skips Flock's check, and a game not hosting through Travel To Session ----

FLOCK_JOIN_TEST(FFlockJoinCheckMissingTest, "Host.AGameModeSkippingFlocksCheck_LetsNobodyIn_AndIsToldOnce")
bool FFlockJoinCheckMissingTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	// The engine's PreLogin event of a game mode whose PreLoginAsync is the engine's own.
	FString Refusal;
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Refusal);
	FString Reason;
	TestTrue(TEXT("turned away by Flock's check"), FlockHostRefusals::TryRead(Refusal, Reason));
	TestEqualSensitive(TEXT("join_check_missing"), Reason, FString(FlockTravelToSessionRefusedReasons::JoinCheckMissing));
	FString Again;
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Again);
	TestFalse(TEXT("the next one too"), Again.IsEmpty());
	TestEqual(TEXT("told once, naming the fix"), F.ErrorsContaining(TEXT("FlockJoinChecks::CheckJoiningPlayer")), 1);

	FString Full = TEXT("Server full.");
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Full);
	TestEqualSensitive(TEXT("the engine's own refusal stands"), Full, FString(TEXT("Server full.")));

	F.Game->bThisGame = false;
	FString Other;
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Other);
	TestTrue(TEXT("another game's game mode is left alone"), Other.IsEmpty());
	F.Game->bThisGame = true;

	// Flock's own check runs the engine's checks, whose PreLogin event is its own.
	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining Checked = F.Joins(WithToken(Token), nullptr, [&F]()
	{
		FString Raised;
		F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Raised);
		return Raised;
	});
	TestTrue(TEXT("a player through Flock's check is let in"), Checked.Answer->LetIn());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinNotHostingTest, "Host.WhileNotHostingThroughTravelToSession_TheEnginesOwnChecksAlone")
bool FFlockJoinNotHostingTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	F.SignIn(PlayerA);
	const FJoining Plain = F.Joins(WithToken(TEXT("made-up")));
	TestTrue(TEXT("let in by the engine's own checks"), Plain.Answer->LetIn());
	TestEqual(TEXT("the engine's checks ran"), F.EngineAsked, 1);
	const FJoining Full = F.Joins(TEXT("?Name=Ann"), nullptr, []() { return FString(TEXT("Server full.")); });
	TestEqualSensitive(TEXT("the engine's words as they are"), Full.Answer->Refusal, FString(TEXT("Server full.")));
	TestEqual(TEXT("Flock was not asked"), F.Server->Count(ERoute::Verify), 0);
	TestEqual(TEXT("nor the game's own check"), F.GameAsked.Num(), 0);
	FString Untouched;
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Untouched);
	TestTrue(TEXT("a game mode skipping the check is left alone while nothing is hosted"), Untouched.IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinConnectionNotFoundTest, "Host.AConnectionTheEngineDoesNotHave_IsRefused_AndSaidWhy")
bool FFlockJoinConnectionNotFoundTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	// The game mode passes options of its own making, so no waiting connection asked with them.
	const FJoining Changed = F.Joins(WithToken(Token), nullptr, nullptr, WithToken(Token) + TEXT("?Extra=1"));
	TestEqualSensitive(TEXT("refused"), Changed.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::JoinCheckMissing));
	TestEqual(TEXT("said why, naming the fix"), F.WarningsContaining(TEXT("with the Options and Address the engine gave it, unchanged")), 1);
	TestEqual(TEXT("Flock was not asked"), F.Server->Count(ERoute::Verify), 0);
	return true;
}

// ---- the check's lifetime ----

FLOCK_JOIN_TEST(FFlockJoinStopListeningTest, "Host.WhenTheHostsLevelStopsListening_TheCheckEnds_AndRemembersNothing")
bool FFlockJoinStopListeningTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining Player = F.Joins(WithToken(Token));
	F.PlaysThrough(Player.Connection);
	TestFalse(TEXT("precondition: known"), Session->PlayerForConnection(JoinedController()).IsEmpty());

	F.Game->StopListening();
	F.Frame();
	TestFalse(TEXT("the check ended"), F.Checks().IsHostingForTesting());
	TestTrue(TEXT("nothing remembered"), Session->PlayerForConnection(JoinedController()).IsEmpty());
	TestTrue(TEXT("nor the host's own player"), Session->PlayerForConnection(HostsOwnController()).IsEmpty());

	// Hosting again starts afresh.
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	F.Game->Opens(7778);
	F.FramesUntil([&]() { return Again->bAnswered; });
	const FJoining Next = F.Joins(WithToken(Token));
	TestTrue(TEXT("let in by the new hosting"), Next.Answer->LetIn());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinPlayerLeftWhileCheckedTest, "Host.AnAnswerLandingAfterThePlayerLeft_LetsNobodyIn_AndRemembersNothing")
bool FFlockJoinPlayerLeftWhileCheckedTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Checking = F.Server->HoldNext(ERoute::Verify);
	const FJoining Player = F.Joins(WithToken(Token));
	F.Game->Leaves(Player.Connection);
	Checking->Release();
	TestTrue(TEXT("answered, so nothing is left waiting"), Player.Answer->bAnswered);
	TestFalse(TEXT("but nobody let in"), Player.Answer->LetIn());
	TestEqual(TEXT("neither the engine nor the game asked"), F.EngineAsked + F.GameAsked.Num(), 0);
	TestEqual(TEXT("and nobody refused for Flock's silence"), F.WarningsContaining(TEXT("Refused a joining player")), 0);
	F.Game->Here.Add(Player.Connection);
	F.PlaysThrough(Player.Connection);
	TestTrue(TEXT("nothing remembered"), Session->PlayerForConnection(JoinedController()).IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinHostStoppedWhileCheckingTest, "Host.AnAnswerLandingAfterTheHostStopped_ChangesNothing")
bool FFlockJoinHostStoppedWhileCheckingTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	UFlockMultiplayerSession* Session = F.HostingAsA(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Checking = F.Server->HoldNext(ERoute::Verify);
	// A player the stopped host still has, so only the stop itself can turn the late answer away.
	const FJoining Player = F.Joins(WithToken(Token));
	F.Game->StopListening();
	F.Frame();
	TestEqualSensitive(TEXT("turned away when hosting ended, not left waiting"), Player.Answer->RefusedFor(),
		FString(FlockTravelToSessionRefusedReasons::FlockUnreachable));
	TestTrue(TEXT("the check given up"), !Checking->bArrived || Checking->bDelivered);

	Checking->Release();
	F.Frame();
	TestEqual(TEXT("answered once"), Player.Answer->Times, 1);
	TestEqual(TEXT("neither the engine nor the game asked"), F.EngineAsked + F.GameAsked.Num(), 0);
	TestEqual(TEXT("nobody refused for Flock's silence"), F.WarningsContaining(TEXT("Refused a joining player")), 0);
	F.PlaysThrough(Player.Connection);
	TestTrue(TEXT("nothing remembered"), Session->PlayerForConnection(JoinedController()).IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinShutdownTest, "Host.FlockShuttingDown_TurnsAwayEveryWaitingPlayer")
bool FFlockJoinShutdownTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Checking = F.Server->HoldNext(ERoute::Verify);
	const FJoining Player = F.Joins(WithToken(Token));
	F.Provider->Shutdown();
	// The engine never drops a waiting player itself (measured), so one left unanswered would wait for ever.
	TestEqualSensitive(TEXT("turned away"), Player.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::FlockUnreachable));
	Checking->Release();
	TestEqual(TEXT("answered once"), Player.Answer->Times, 1);

	// The level stays open: nobody can be checked now, so every player is turned away, whatever the game mode does.
	FString Next;
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Next);
	FString Reason;
	TestTrue(TEXT("a player after the shutdown turned away"), FlockHostRefusals::TryRead(Next, Reason));
	TestEqualSensitive(TEXT("as Flock unreachable"), Reason, FString(FlockTravelToSessionRefusedReasons::FlockUnreachable));
	FString Another;
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Another);
	TestEqual(TEXT("told once, naming what to do"), F.WarningsContaining(TEXT("Leave the hosted level before shutting Flock down")), 1);
	FTSTicker::GetCoreTicker().Tick(0.f);
	TestTrue(TEXT("still turning players away while the level stays open"), F.Checks().IsHostingForTesting());

	// Once the level closes, the engine's own checks decide again.
	F.Game->StopListening();
	FTSTicker::GetCoreTicker().Tick(0.f);
	TestFalse(TEXT("the check ended once the level closed"), F.Checks().IsHostingForTesting());
	FString Later;
	F.Checks().TakePreLoginEvent(nullptr, FUniqueNetIdRepl(), Later);
	TestTrue(TEXT("and left alone"), Later.IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinShutdownLevelClosedTest, "Host.FlockShuttingDownWithTheLevelClosed_EndsTheCheckAtOnce")
bool FFlockJoinShutdownLevelClosedTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	F.Game->StopListening();
	F.Provider->Shutdown();
	TestFalse(TEXT("nothing left turning players away"), F.Checks().IsHostingForTesting());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinShutdownThenHostedAgainTest, "Host.AfterAShutdown_HostingAgainInAnotherLevel_EndsTheOldRefusals")
bool FFlockJoinShutdownThenHostedAgainTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	if (F.HostingAsA(*this) == nullptr)
	{
		return false;
	}
	F.Provider->Shutdown();
	TestTrue(TEXT("precondition: turning players away"), F.Checks().IsHostingForTesting());
	// The game opens another level listening (as hosting again does), with no frame in between.
	F.Game->State.bInAnotherWorld = true;
	FTSTicker::GetCoreTicker().Tick(0.f);
	TestFalse(TEXT("the old level's refusals ended"), F.Checks().IsHostingForTesting());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinTravelDidNotHostTest, "Host.AHostTravelThatDoesNotStartHosting_EndsTheCheck")
bool FFlockJoinTravelDidNotHostTest::RunTest(const FString& Parameters)
{
	{
		FJoinFixture F;
		F.SignIn(PlayerA);
		UFlockMultiplayerSession* Session = F.Hosted(*this);
		const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
		F.Frame();
		TestTrue(TEXT("checking while the level opens"), F.Checks().IsHostingForTesting());
		F.Game->Opens(0);
		F.Frame();
		TestEqualSensitive(TEXT("could not start"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotStart));
		TestFalse(TEXT("the check ended with it"), F.Checks().IsHostingForTesting());
	}
	{
		FJoinFixture F;
		F.SignIn(PlayerA);
		UFlockMultiplayerSession* Session = F.Hosted(*this);
		FFlockRequestHandle Handle;
		F.Travel(Session, FFlockTravelToSessionOptions(), &Handle);
		Handle.Cancel();
		TestTrue(TEXT("checking until the level it opens stops listening"), F.Checks().IsHostingForTesting());
		F.Game->Opens(7777);
		F.Frame();
		TestFalse(TEXT("the check ended with the cancelled travel"), F.Checks().IsHostingForTesting());
	}
	{
		FJoinFixture F;
		F.SignIn(PlayerA);
		UFlockMultiplayerSession* Session = F.Hosted(*this);
		F.Server->AnswerNext(ERoute::Publish, FFlockFakeSessionServer::Refused(403, TEXT("multiplayer.not_host")));
		const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
		F.Game->Opens(7777);
		F.FramesUntil([&]() { return Travel->bAnswered; });
		TestTrue(TEXT("the publish refused"), Travel->bAnswered && !Travel->Result.bSuccess);
		F.Frame();
		TestFalse(TEXT("the host stopped listening, which ends the check"), F.Checks().IsHostingForTesting());
	}
	{
		FJoinFixture F;
		F.SignIn(PlayerA);
		UFlockMultiplayerSession* Session = F.Hosted(*this);
		const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
		F.Game->Fails(TEXT("ClientTravelFailure: Failed to load package '/Game/Maps/Arena'"));
		F.Game->Opens(7777);
		F.Frame();
		TestEqualSensitive(TEXT("a level that failed to open could not start"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotStart));
		TestTrue(TEXT("while the game still listens, its players are still checked"), F.Checks().IsHostingForTesting());
		F.Game->GoesBackToItsDefaultLevel();
		F.Frame();
		TestFalse(TEXT("the check ends once it no longer listens"), F.Checks().IsHostingForTesting());
	}
	{
		FJoinFixture F;
		F.SignIn(PlayerA);
		UFlockMultiplayerSession* Session = F.Hosted(*this);
		const TSharedRef<FFlockFakePartyServer::FHeld> Publishing = F.Server->HoldNext(ERoute::Publish);
		FFlockRequestHandle Handle;
		F.Travel(Session, FFlockTravelToSessionOptions(), &Handle);
		F.Game->Opens(7777);
		F.FramesUntil([&]() { return Publishing->bArrived; });
		Handle.Cancel();
		F.Frame();
		TestFalse(TEXT("a travel cancelled while publishing stops listening, which ends the check"), F.Checks().IsHostingForTesting());
	}
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinNotHostedHereTest, "Host.PlayerForConnection_OnASessionNotHostedHere_IsNobody")
bool FFlockJoinNotHostedHereTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Game->OwnPlayers.Add(HostsOwnController());
	TestTrue(TEXT("not hosting through Travel To Session: nobody"), Session->PlayerForConnection(HostsOwnController()).IsEmpty());
	TestTrue(TEXT("no controller: nobody"), Session->PlayerForConnection(nullptr).IsEmpty());

	// Hosting a newer session ends this one (moved_to_another_session); the session hosted through Travel To Session is the newer one.
	UFlockMultiplayerSession* Newer = F.HostingAsA(*this);
	if (Newer == nullptr || Newer == Session)
	{
		return false;
	}
	TestEqualSensitive(TEXT("the session hosted here"), Newer->PlayerForConnection(HostsOwnController()), PlayerA);
	TestTrue(TEXT("an earlier session of the player's is nobody's"), Session->PlayerForConnection(HostsOwnController()).IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinWithoutFlockTest, "Host.AGameWithoutFlock_TheGameModesCallIsTheEnginesOwn")
bool FFlockJoinWithoutFlockTest::RunTest(const FString& Parameters)
{
	// A game of its own whose Flock never started (its game instance made, never initialized), and the game mode the engine makes for it.
	UGameInstance* Instance = NewObject<UGameInstance>(GetTransientPackage());
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("FlockJoinChecksWithoutFlock"));
	if (!TestNotNull(TEXT("precondition: a world"), World))
	{
		return false;
	}
	// A world context of its own: under -game the engine asks for the world's context while it makes the game mode.
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.OwningGameInstance = Instance;
	Context.SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	};
	World->SetGameInstance(Instance);
	if (!TestTrue(TEXT("precondition: the engine made a game mode"), World->SetGameMode(FURL())))
	{
		return false;
	}
	AGameModeBase* GameMode = World->GetAuthGameMode();
	FString Unused;
	GameMode->InitGame(TEXT("FlockJoinChecksWithoutFlock"), FString(), Unused);
	TestNull(TEXT("precondition: no Flock running in this game"), UFlockSubsystem::Get(GameMode));

	TArray<FString> Answers;
	int32 GamesChecks = 0;
	FlockJoinChecks::CheckJoiningPlayer(GameMode, TEXT("?FlockJoinToken=made-up?Name=Ann"), TEXT("127.0.0.1"), FUniqueNetIdRepl(),
		AGameModeBase::FOnPreLoginCompleteDelegate::CreateLambda([&Answers](const FString& Refusal) { Answers.Add(Refusal); }),
		[&GamesChecks](const FFlockJoiningPlayer&, FString&) { ++GamesChecks; return false; });
	TestEqual(TEXT("answered at once"), Answers.Num(), 1);
	TestEqualSensitive(TEXT("by the engine's own checks: let in"), Answers.Num() == 1 ? Answers[0] : FString(TEXT("<no answer>")), FString());
	TestEqual(TEXT("the game's own check not run"), GamesChecks, 0);
	return true;
}

// ---- the joining player ----

FLOCK_JOIN_TEST(FFlockJoinTokenInTravelTest, "Player.TravelToSession_AsksForAJoinTokenOnceTheHostIsKnown_AndCarriesItToTheHost")
bool FFlockJoinTokenInTravelTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	FFlockFakeSessionServer::FSessionRef Served = F.Served;
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
	F.Advance(0.25);
	TestEqual(TEXT("no token while the host is unknown: a token lasts 60 s"), F.Server->Count(ERoute::JoinToken), 0);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqual(TEXT("one token asked for"), F.Server->Count(ERoute::JoinToken), 1);
	TestEqualSensitive(TEXT("the token Flock gave this player for this session"), FString::Join(F.Game->TokensTraveledWith, TEXT(",")),
		FString::Printf(TEXT("token-%s-%s-1"), *Served->Id, *PlayerB));
	F.Game->Arrives(true);
	F.Frame();
	TestEqualSensitive(TEXT("connected"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Connected));
	TestTrue(TEXT("no reason"), Travel->Result.Value.RefusedReason.IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinRefusedPlayerTest, "Player.APlayerTheHostTurnsAway_IsRefused_WithTheHostsReason")
bool FFlockJoinRefusedPlayerTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Reasons = { FlockTravelToSessionRefusedReasons::JoinTokenInvalid, TEXT("banned"), TEXT("Server full.") };
	for (const FString& Reason : Reasons)
	{
		FJoinFixture F;
		FFlockFakeSessionServer::FSessionRef Served = F.Served;
		UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
		if (Session == nullptr)
		{
			return false;
		}
		F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
		const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
		F.Advance(0.25);
		F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
		// As measured: the waiting connection fails with the host's words, then the engine loads the default level.
		F.Game->Fails(FString(TEXT("PendingConnectionFailure: ")) + FlockHostRefusals::WordsFor(Reason));
		F.Frame();
		TestFalse(*FString::Printf(TEXT("%s: the engine settles first"), *Reason), Travel->bAnswered);
		F.Game->GoesBackToItsDefaultLevel();
		F.Frame();
		TestEqualSensitive(*FString::Printf(TEXT("%s: refused"), *Reason), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Refused));
		TestEqualSensitive(*FString::Printf(TEXT("%s: the host's reason"), *Reason), Travel->Result.Value.RefusedReason, Reason);
		TestEqual(*FString::Printf(TEXT("%s: said why"), *Reason), F.WarningsContaining(FString::Printf(TEXT("turned this player away (%s)"), *Reason)), 1);
	}
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinRefusalHeardEarlierTest, "Player.ARefusalHeardBeforeTheConnectionBegan_SaysNothingOfIt")
bool FFlockJoinRefusalHeardEarlierTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	FFlockFakeSessionServer::FSessionRef Served = F.Served;
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
	F.Advance(0.25);
	// Words of a refusal, heard while the travel waits for the host: an earlier connection of the game's being dropped.
	F.Game->Fails(FString(TEXT("PendingConnectionFailure: ")) + FlockHostRefusals::WordsFor(TEXT("banned")));
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	// This connection is given up with no word from the engine.
	F.Game->GoesBackToItsDefaultLevel();
	F.Frame();
	TestEqualSensitive(TEXT("could not connect, not refused"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestTrue(TEXT("no reason"), Travel->Result.Value.RefusedReason.IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinHostSideRefusedTest, "Host.AHostThatCannotCheckItsPlayers_DoesNotOpenItsLevel")
bool FFlockJoinHostSideRefusedTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	// The engine side the check of joining players keeps is the second one the travel makes.
	F.RefuseGameSide = 2;
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
	TestTrue(TEXT("refused at once"), Travel->FailedAs(EFlockErrorType::Validation));
	TestFalse(TEXT("with a fix"), Travel->Result.Error.Hint.IsEmpty());
	TestEqual(TEXT("no level opened"), F.Game->Opened.Num(), 0);
	TestFalse(TEXT("no check started"), F.Checks().IsHostingForTesting());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinHostClosedTest, "Player.AHostClosingItsConnections_IsCouldNotConnect_NotARefusal")
bool FFlockJoinHostClosedTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	FFlockFakeSessionServer::FSessionRef Served = F.Served;
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	// Measured: a host stopping its net driver fails a waiting connection the same way, with the engine's own words.
	F.Game->Fails(TEXT("PendingConnectionFailure: Host closed the connection."));
	F.Game->GoesBackToItsDefaultLevel();
	F.Frame();
	TestEqualSensitive(TEXT("could not connect"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestTrue(TEXT("no reason"), Travel->Result.Value.RefusedReason.IsEmpty());
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinNoTokenGivenTest, "Player.APlayerFlockGivesNoToken_DoesNotTravel_AndIsToldWhy")
bool FFlockJoinNoTokenGivenTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	FFlockFakeSessionServer::FSessionRef Served = F.Served;
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Server->AnswerNext(ERoute::JoinToken, FFlockFakeSessionServer::Refused(404, TEXT("multiplayer.not_a_participant")));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
	F.Advance(0.25);
	F.FramesUntil([&]() { return Travel->bAnswered; });
	TestTrue(TEXT("refused by Flock, on its code"), Travel->bAnswered && !Travel->Result.bSuccess
		&& Travel->Result.Error.ErrorCode == EFlockErrorCode::MultiplayerNotAParticipant);
	TestFalse(TEXT("with a fix"), Travel->Result.Error.Hint.IsEmpty());
	TestEqual(TEXT("nothing traveled without a token"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinCancelWhileAskingTest, "Player.CancellingWhileTheTokenIsAskedFor_TravelsNowhere")
bool FFlockJoinCancelWhileAskingTest::RunTest(const FString& Parameters)
{
	FJoinFixture F;
	FFlockFakeSessionServer::FSessionRef Served = F.Served;
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	const TSharedRef<FFlockFakePartyServer::FHeld> Asking = F.Server->HoldNext(ERoute::JoinToken);
	FFlockRequestHandle Handle;
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, FFlockTravelToSessionOptions(), &Handle);
	F.Advance(0.25);
	F.FramesUntil([&]() { return Asking->bArrived; });
	Handle.Cancel();
	TestTrue(TEXT("cancelled"), Travel->FailedAs(EFlockErrorType::Cancelled));
	TestTrue(TEXT("the token's request given up, not only its answer ignored"), Asking->bDelivered);
	Asking->Release();
	F.Frame();
	TestEqual(TEXT("answered once"), Travel->Times, 1);
	TestEqual(TEXT("traveled nowhere"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_JOIN_TEST(FFlockJoinRefusalWordsTest, "Player.ARefusalReadsBackOnlyFromAHostsCheck")
bool FFlockJoinRefusalWordsTest::RunTest(const FString& Parameters)
{
	FString Reason;
	TestTrue(TEXT("a host's check's words"), FlockHostRefusals::TryRead(FlockHostRefusals::WordsFor(TEXT("banned")), Reason));
	TestEqualSensitive(TEXT("the reason"), Reason, FString(TEXT("banned")));
	TestFalse(TEXT("the engine's own"), FlockHostRefusals::TryRead(TEXT("Host closed the connection."), Reason));
	TestFalse(TEXT("no reason after the beginning"), FlockHostRefusals::TryRead(FlockHostRefusals::Beginning, Reason));
	TestFalse(TEXT("letter case matters"), FlockHostRefusals::TryRead(FString(FlockHostRefusals::Beginning).ToLower() + TEXT("banned"), Reason));
	for (const TCHAR* Each : { FlockTravelToSessionRefusedReasons::NoJoinToken, FlockTravelToSessionRefusedReasons::JoinTokenInvalid,
		FlockTravelToSessionRefusedReasons::AlreadyConnected, FlockTravelToSessionRefusedReasons::FlockUnreachable,
		FlockTravelToSessionRefusedReasons::SessionEnded, FlockTravelToSessionRefusedReasons::NotHost,
		FlockTravelToSessionRefusedReasons::RefusedByTheGame, FlockTravelToSessionRefusedReasons::JoinCheckMissing })
	{
		TestFalse(*FString::Printf(TEXT("%s has a fix of its own"), Each), FlockHostRefusals::FixFor(Each) == FlockHostRefusals::FixFor(TEXT("banned")));
	}
	TestTrue(TEXT("a game's own reason in another case is the game's own"),
		FlockHostRefusals::FixFor(TEXT("NO_JOIN_TOKEN")) == FlockHostRefusals::FixFor(TEXT("banned")));
	return true;
}

// ---- the engine's own side, where it runs ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJoinEngineSideTest, "Flock.Multiplayer.JoinVerification.Host.TheEngineSideReadsTheRunningGame_AndItsPreLoginEvent",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockJoinEngineSideTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World() != nullptr)
		{
			World = Context.World();
		}
	}
	if (World == nullptr || World->GetAuthGameMode() == nullptr)
	{
		AddInfo(TEXT("No game world here (the editor's pass): the engine side runs under -game."));
		return true;
	}
	AGameModeBase* GameMode = World->GetAuthGameMode();
	APlayerController* Own = World->GetFirstPlayerController();
	FString Refusal;
	const TSharedPtr<IFlockGameTravel> Game = FlockEngineTravel()(World, Refusal);
	if (!TestTrue(TEXT("the running game's engine side"), Game.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("its game mode is this game's"), Game->IsThisGame(GameMode));
	TestFalse(TEXT("no game mode is nobody's"), Game->IsThisGame(nullptr));
	TestTrue(TEXT("its local player is its own"), Own != nullptr && Game->IsOwnPlayer(Own));
	TestTrue(TEXT("a local player plays through no connection"), Game->ConnectionOf(Own) == 0);
	TestTrue(TEXT("no connection waits while it does not listen"), Game->FindJoiningConnection(TEXT("?Name=Ann"), TEXT("127.0.0.1")) == 0);
	TestFalse(TEXT("a number never handed out is not here"), Game->IsStillHere(1));

	// The engine's PreLogin event, raised by the running game mode itself, reaches a hosting check.
	FJoinFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Checks().StartHosting(Session, Game.ToSharedRef());
	FString Skipped;
	GameMode->PreLogin(TEXT("?Name=Ann"), TEXT("127.0.0.1"), FUniqueNetIdRepl(), Skipped);
	FString Reason;
	TestTrue(TEXT("a game mode that skips Flock's check lets nobody in"), FlockHostRefusals::TryRead(Skipped, Reason)
		&& Reason.Equals(FlockTravelToSessionRefusedReasons::JoinCheckMissing, ESearchCase::CaseSensitive));
	F.Checks().StopHosting();
	FString Plain;
	GameMode->PreLogin(TEXT("?Name=Ann"), TEXT("127.0.0.1"), FUniqueNetIdRepl(), Plain);
	TestTrue(TEXT("left alone once hosting ends"), Plain.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJoinPublicEntryTest, "Flock.Multiplayer.JoinVerification.Host.TheGameModesCall_ReachesTheRunningGamesFlock",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockJoinPublicEntryTest::RunTest(const FString& Parameters)
{
	TArray<FString> Answers;
	const AGameModeBase::FOnPreLoginCompleteDelegate Record = AGameModeBase::FOnPreLoginCompleteDelegate::CreateLambda([&Answers](const FString& Refusal)
	{
		Answers.Add(Refusal);
	});
	AddExpectedError(TEXT("was given no game mode"), EAutomationExpectedErrorFlags::Contains, 1);
	FlockJoinChecks::CheckJoiningPlayer(nullptr, TEXT("?Name=Ann"), TEXT("127.0.0.1"), FUniqueNetIdRepl(), Record);
	FString Reason;
	TestTrue(TEXT("no game mode: turned away, never left waiting"), Answers.Num() == 1 && FlockHostRefusals::TryRead(Answers[0], Reason)
		&& Reason.Equals(FlockTravelToSessionRefusedReasons::JoinCheckMissing, ESearchCase::CaseSensitive));

	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World() != nullptr)
		{
			World = Context.World();
		}
	}
	if (World == nullptr || World->GetAuthGameMode() == nullptr)
	{
		AddInfo(TEXT("No game world here (the editor's pass): the game mode's call runs under -game."));
		return true;
	}
	// The running game hosts nothing through Travel To Session, so its game mode's call is the engine's own PreLoginAsync, answered at once.
	Answers.Reset();
	FlockJoinChecks::CheckJoiningPlayer(World->GetAuthGameMode(), TEXT("?FlockJoinToken=made-up?Name=Ann"), TEXT("127.0.0.1"), FUniqueNetIdRepl(), Record,
		[](const FFlockJoiningPlayer&, FString& OutReason) { OutReason = TEXT("the game's check ran"); return false; });
	TestEqual(TEXT("answered at once"), Answers.Num(), 1);
	TestTrue(TEXT("by the engine's own checks, the game's not run"), Answers.Num() == 1 && Answers[0].IsEmpty());
	return true;
}

namespace FlockJoinVerificationTestHelpers
{
	/** Frame times in milliseconds over a number of real frames: their median, 95th percentile and longest. */
	struct FFrameTimes
	{
		TArray<double> Samples;
		double Last = 0.0;

		void Take()
		{
			const double Now = FPlatformTime::Seconds();
			if (Last > 0.0)
			{
				Samples.Add((Now - Last) * 1000.0);
			}
			Last = Now;
		}
		double At(double Share) const
		{
			TArray<double> Sorted = Samples;
			Sorted.Sort();
			return Sorted.Num() > 0 ? Sorted[FMath::Min(Sorted.Num() - 1, FMath::FloorToInt(Share * Sorted.Num()))] : 0.0;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockJoinTenChecksFramesTest, "Flock.Multiplayer.JoinVerification.Host.TenChecksWaitingOnFlock_TheHostsFramesKeepTheirPace",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockJoinTenChecksFramesTest::RunTest(const FString& Parameters)
{
	// Real frames: each latent step runs once an engine frame.
	const TSharedRef<FJoinFixture> F = MakeShared<FJoinFixture>();
	if (F->HostingAsA(*this) == nullptr)
	{
		return false;
	}
	const TSharedRef<TArray<TSharedRef<FFlockFakePartyServer::FHeld>>> Held = MakeShared<TArray<TSharedRef<FFlockFakePartyServer::FHeld>>>();
	const TSharedRef<TArray<FJoining>> Joinings = MakeShared<TArray<FJoining>>();
	const TSharedRef<FFrameTimes> Before = MakeShared<FFrameTimes>();
	const TSharedRef<FFrameTimes> During = MakeShared<FFrameTimes>();
	const int32 FramesEach = 60;

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([F, Before, FramesEach]()
	{
		F->Frame();
		Before->Take();
		return Before->Samples.Num() >= FramesEach;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, F, Held, Joinings]()
	{
		const double Asking = FPlatformTime::Seconds();
		for (int32 Index = 0; Index < 10; ++Index)
		{
			const FString Token = F->SeatedWithToken(FString::Printf(TEXT("player-%d"), Index));
			Held->Add(F->Server->HoldNext(FFlockFakeSessionServer::ERoute::Verify));
			Joinings->Add(F->Joins(WithToken(Token)));
		}
		TestTrue(TEXT("the ten questions came back at once, inside a frame"), FPlatformTime::Seconds() - Asking < 0.25);
		TestEqual(TEXT("ten checks waiting on Flock"), F->Checks().GetChecksWaitingForTesting(), 10);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, F, During, Before, Held, Joinings, FramesEach]()
	{
		F->Frame();
		During->Take();
		if (During->Samples.Num() < FramesEach)
		{
			return false;
		}
		AddInfo(FString::Printf(TEXT("Frames without checks: p50 %.2f ms, p95 %.2f ms, longest %.2f ms; with ten waiting: p50 %.2f ms, p95 %.2f ms, longest %.2f ms."),
			Before->At(0.5), Before->At(0.95), Before->At(1.0), During->At(0.5), During->At(0.95), During->At(1.0)));
		TestTrue(TEXT("no frame waited on a check"), During->At(1.0) < 250.0);
		TestTrue(TEXT("still waiting on Flock"), !Joinings->ContainsByPredicate([](const FJoining& Each) { return Each.Answer->bAnswered; }));
		for (const TSharedRef<FFlockFakePartyServer::FHeld>& Check : *Held)
		{
			Check->Release();
		}
		TestTrue(TEXT("each let in on Flock's answer"), !Joinings->ContainsByPredicate([](const FJoining& Each) { return !Each.Answer->LetIn(); }));
		return true;
	}));
	return true;
}

#endif
