// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Config/FlockConfig.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "FlockInitConfig.h"
#include "FlockSubsystem.h"
#include "Http/FlockErrorHints.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakeSessionServer.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockSessionTestListener.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "UObject/StrongObjectPtr.h"

namespace FlockSessionsTestHelpers
{
	using ERoute = FFlockFakeSessionServer::ERoute;

	const FString SessionPlayerA = TEXT("player-a");
	const FString SessionPlayerB = TEXT("player-b");
	const FString SessionPlayerC = TEXT("player-c");
	const FString SessionPlayerD = TEXT("player-d");

	/** A JWT naming PlayerId as its subject; the fake server reads the requester from it. */
	inline FString SessionJwt(const FString& PlayerId, int32 Variant = 0)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600 + Variant;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/** What a session call answered; bAnswered stays false while its request is held. */
	template <typename T>
	struct TCallAnswer
	{
		bool bAnswered = false;
		TFlockResult<T> Result;

		bool Succeeded() const { return bAnswered && Result.bSuccess; }
		EFlockErrorType FailureType() const { return Result.Error.Type; }
		EFlockErrorCode FailureCode() const { return Result.Error.ErrorCode; }
		bool FailedWith(EFlockErrorCode Code) const { return bAnswered && !Result.bSuccess && Result.Error.ErrorCode == Code; }
	};
	using FSessionAnswer = TCallAnswer<UFlockMultiplayerSession*>;
	using FSessionAnswerRef = TSharedRef<FSessionAnswer>;

	template <typename T>
	TFunction<void(TFlockResult<T>)> Into(const TSharedRef<TCallAnswer<T>>& Answer)
	{
		return [Answer](TFlockResult<T> Result)
		{
			Answer->bAnswered = true;
			Answer->Result = Result;
		};
	}

	inline FSessionAnswerRef NewAnswer() { return MakeShared<FSessionAnswer>(); }

	inline FString PlayersOf(const UFlockMultiplayerSession* Session)
	{
		TArray<FString> Ids;
		if (Session != nullptr)
		{
			for (const FFlockMultiplayerSessionPlayer& Player : Session->GetPlayers())
			{
				Ids.Add(Player.PlayerId);
			}
		}
		return FString::Join(Ids, TEXT(","));
	}

	inline TSharedPtr<FJsonObject> SessionBodyOf(const FFlockHttpRequest& Request)
	{
		TSharedPtr<FJsonObject> Body;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
		return Body;
	}

	inline TSharedRef<FJsonObject> DirectConnection(const FString& Address)
	{
		const TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetStringField(TEXT("mode"), TEXT("direct"));
		Connection->SetStringField(TEXT("address"), Address);
		Connection->SetNumberField(TEXT("port"), 7777);
		return Connection;
	}

	/** The session stack as the provider builds it, over a fake session server, with time moved by hand. */
	struct FSessionsFixture
	{
		TSharedRef<double> Now = MakeShared<double>(1000.0);
		TSharedRef<FFlockFakeSessionServer> Server = MakeShared<FFlockFakeSessionServer>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockAuthSession> Session;
		TSharedRef<FFlockMultiplayerProvider> Provider;
		TArray<TStrongObjectPtr<UFlockSessionTestListener>> Listeners;
		/** Every session a call answered with, kept past its end as a game would in a UPROPERTY, whenever the answer lands. */
		TSharedRef<TArray<TStrongObjectPtr<UFlockMultiplayerSession>>> Kept = MakeShared<TArray<TStrongObjectPtr<UFlockMultiplayerSession>>>();

		static FFlockRetryPolicy Retries(int32 MaxRetries)
		{
			FFlockRetryPolicy Policy;
			Policy.MaxRetries = MaxRetries;
			Policy.InitialDelaySeconds = 0.f;
			Policy.bUseJitter = false;
			return Policy;
		}

		explicit FSessionsFixture(int32 MaxRetries = 0)
			: Client(MakeShared<FFlockHttpClient>(Server, Log))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), Log, TEXT("http://x/v1"),
				TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") }, { TEXT("X-Game-Version-ID"), FFlockFakeSessionServer::GameBuild } }))
			, Provider(MakeShared<FFlockMultiplayerProvider>(Client, Retries(MaxRetries), Log, Session, TEXT("http://x/v1"), 0.0))
		{
			const TSharedRef<double> Clock = Now;
			Provider->GetRepeatingCalls().SetClockForTesting([Clock]() { return *Clock; }, []() { return 0.5; });
		}

		~FSessionsFixture()
		{
			Provider->Shutdown();
		}

		void SignIn(const FString& PlayerId, int32 Variant = 0)
		{
			FString Error;
			Session->SetTokens(SessionJwt(PlayerId, Variant), TEXT("r-1"), Error);
		}

		void SignOut() { Session->ClearTokens(); }

		/** One frame of the provider: a session of an ended sign-in ends, then the calls due are sent. */
		void Frame() { Provider->TickForTesting(); }

		/** Moves time on and runs a frame; a heartbeat due by then is sent, and answered unless held. */
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

		FFlockMultiplayerSessions& Sessions() { return Provider->GetSessionsForTesting(); }
		UFlockMultiplayerSession* Held() { return Sessions().GetHeld(); }

		FSessionAnswerRef Host(TOptional<int32> MaxPlayers = TOptional<int32>(), const FFlockCommandData& Data = FFlockCommandData(),
			bool bSameVersionOnly = true)
		{
			const FSessionAnswerRef Answer = NewAnswer();
			Provider->HostSession(MaxPlayers, Data, bSameVersionOnly, KeepingInto(Answer));
			return Answer;
		}

		FSessionAnswerRef Join(const FString& Code)
		{
			const FSessionAnswerRef Answer = NewAnswer();
			Provider->JoinSession(Code, KeepingInto(Answer));
			return Answer;
		}

		FSessionAnswerRef Get(const FString& SessionId)
		{
			const FSessionAnswerRef Answer = NewAnswer();
			Provider->GetSession(SessionId, KeepingInto(Answer));
			return Answer;
		}

		FSessionAnswerRef GetMine()
		{
			const FSessionAnswerRef Answer = NewAnswer();
			Provider->GetMySession(KeepingInto(Answer));
			return Answer;
		}

		/** A session this test hosted and kept, or null with the test failed. */
		UFlockMultiplayerSession* Hosted(FAutomationTestBase& Test, TOptional<int32> MaxPlayers = TOptional<int32>(),
			const FFlockCommandData& Data = FFlockCommandData())
		{
			const FSessionAnswerRef Made = Host(MaxPlayers, Data);
			Test.TestTrue(TEXT("precondition: the session was hosted"), Made->Succeeded() && Made->Result.Value != nullptr);
			return Made->Result.Value;
		}

		/** A session this test joined by code and kept, or null with the test failed. */
		UFlockMultiplayerSession* Joined(FAutomationTestBase& Test, const FString& Code)
		{
			const FSessionAnswerRef Made = Join(Code);
			Test.TestTrue(TEXT("precondition: the session was joined"), Made->Succeeded() && Made->Result.Value != nullptr);
			return Made->Result.Value;
		}

		UFlockSessionTestListener* Listen(UFlockMultiplayerSession* Of)
		{
			UFlockSessionTestListener* Listener = NewObject<UFlockSessionTestListener>();
			Listeners.Emplace(Listener);
			if (Of != nullptr)
			{
				Listener->Listen(Of);
			}
			return Listener;
		}

		FSessionAnswerRef Leave(UFlockMultiplayerSession* Of) { const FSessionAnswerRef A = NewAnswer(); Of->Leave(Into(A)); return A; }
		FSessionAnswerRef End(UFlockMultiplayerSession* Of) { const FSessionAnswerRef A = NewAnswer(); Of->End(Into(A)); return A; }
		FSessionAnswerRef MakeHost(UFlockMultiplayerSession* Of, const FString& PlayerId)
		{
			const FSessionAnswerRef A = NewAnswer();
			Of->MakeHost(PlayerId, Into(A));
			return A;
		}
		FSessionAnswerRef Publish(UFlockMultiplayerSession* Of, const FString& Mode, const FFlockCommandData& Values = FFlockCommandData())
		{
			const FSessionAnswerRef A = NewAnswer();
			Of->PublishConnection(Mode, Values, Into(A));
			return A;
		}

	private:
		/** Records the answer and keeps its session the moment it lands, a held request's later answer included. */
		FFlockOnMultiplayerSession KeepingInto(const FSessionAnswerRef& Answer) const
		{
			const TSharedRef<TArray<TStrongObjectPtr<UFlockMultiplayerSession>>> KeptSessions = Kept;
			return [Answer, KeptSessions](TFlockResult<UFlockMultiplayerSession*> Result)
			{
				Answer->bAnswered = true;
				Answer->Result = Result;
				if (Result.Value != nullptr)
				{
					KeptSessions->Emplace(Result.Value);
				}
			};
		}
	};

	using FTokenAnswer = TCallAnswer<FFlockMultiplayerSessionJoinToken>;
	using FPlayerAnswer = TCallAnswer<FString>;
}

using namespace FlockSessionsTestHelpers;

#define FLOCK_SESSION_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Sessions." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// ---- getting a session ----

FLOCK_SESSION_TEST(FFlockSessionHostTest, "HostHoldsThePlayerAsHostAndOnlyPlayerAndSendsOnlyWhatWasGiven")
bool FFlockSessionHostTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	const FSessionAnswerRef Hosted = F.Host();
	if (!TestTrue(TEXT("hosted"), Hosted->Succeeded()) || !TestNotNull(TEXT("a session"), Hosted->Result.Value))
	{
		return false;
	}
	UFlockMultiplayerSession* Session = Hosted->Result.Value;
	TestEqualSensitive(TEXT("its only player is the player"), PlayersOf(Session), SessionPlayerA);
	TestTrue(TEXT("the player hosts it"), Session->IsHost());
	TestEqualSensitive(TEXT("the server's id"), Session->GetId(), FString(TEXT("01SESSION00000000000000001")));
	TestEqualSensitive(TEXT("the server's join code"), Session->GetJoinCode(), FString(TEXT("S10001")));
	TestEqual(TEXT("the server's own default size"), Session->GetMaxPlayers(), 8);
	TestFalse(TEXT("no connection before the host publishes"), Session->HasConnection());
	TestFalse(TEXT("not ended"), Session->HasEnded());
	TestTrue(TEXT("no end reason while it lasts"), Session->GetEndReason().IsEmpty());
	if (TestEqual(TEXT("one request"), F.Server->Requests.Num(), 1))
	{
		TestEqualSensitive(TEXT("nothing given, nothing sent"), F.Server->Requests[0].JsonBody, FString(TEXT("{}")));
	}
	TestTrue(TEXT("held"), F.Held() == Session);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHostSendsTest, "HostSendsTheSizeAndDataAsGivenAndGetDataReadsThem")
bool FFlockSessionHostSendsTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this, 4, FFlockCommandData().Set(TEXT("Map"), TEXT("dust")).Set(TEXT("level"), 3));
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Sent = SessionBodyOf(F.Server->Requests[0]);
	int32 SentSize = 0;
	const TSharedPtr<FJsonObject>* SentData = nullptr;
	TestTrue(TEXT("the size was sent"), Sent.IsValid() && Sent->TryGetNumberField(TEXT("max_players"), SentSize) && SentSize == 4);
	TestTrue(TEXT("the data was sent"), Sent.IsValid() && Sent->TryGetObjectField(TEXT("data"), SentData));
	if (SentData != nullptr)
	{
		TestTrue(TEXT("a data key is sent as written"), FlockTestSpelling::HasMemberSpelled(*SentData, TEXT("Map")));
	}
	TestFalse(TEXT("same version only is the server's default, so it is not sent"), Sent.IsValid() && Sent->HasField(TEXT("same_version_only")));
	TestEqual(TEXT("the size held"), Session->GetMaxPlayers(), 4);
	FString Map;
	TestTrue(TEXT("the data reads back"), Session->GetData().TryGetString(TEXT("Map"), Map));
	TestEqualSensitive(TEXT("as sent"), Map, FString(TEXT("dust")));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionJoinTest, "JoinByCodeShowsEveryPlayerLongestSeatedFirst")
bool FFlockSessionJoinTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.Server->Joins(Theirs, SessionPlayerC);
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Joined(*this, TEXT(" s10001 "));
	if (Session == nullptr)
	{
		return false;
	}
	TestEqualSensitive(TEXT("the host, the next, then the player"), PlayersOf(Session), FString(TEXT("player-b,player-c,player-a")));
	TestFalse(TEXT("the player does not host"), Session->IsHost());
	TestEqualSensitive(TEXT("the host"), Session->GetHostPlayerId(), SessionPlayerB);
	TestTrue(TEXT("held"), F.Held() == Session);
	TestEqual(TEXT("no read after the join: its answer is the whole session"), F.Server->Count(ERoute::Read), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionGetHeldTest, "GetSessionReturnsTheHeldObjectAndRefusesASessionNeverSeatedIn")
bool FFlockSessionGetHeldTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FSessionAnswerRef Again = F.Get(Session->GetId());
	TestTrue(TEXT("read"), Again->Succeeded());
	TestTrue(TEXT("the same object"), Again->Result.Value == Session);

	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	const FSessionAnswerRef Other = F.Get(Theirs->Id);
	TestTrue(TEXT("a session never seated in is refused on its code"), Other->FailedWith(EFlockErrorCode::MultiplayerSessionNotFound));
	TestTrue(TEXT("the held one carries on"), F.Held() == Session && !Session->HasEnded());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionGetLeftTest, "GetSessionOfASessionThePlayerLeftComesBackEndedAndIsNotHeld")
bool FFlockSessionGetLeftTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.Server->Joins(Theirs, SessionPlayerA);
	F.Server->Leaves(Theirs, SessionPlayerA);
	F.SignIn(SessionPlayerA);
	const FSessionAnswerRef Read = F.Get(Theirs->Id);
	if (!TestTrue(TEXT("read"), Read->Succeeded() && Read->Result.Value != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("handed back ended"), Read->Result.Value->HasEnded());
	TestEqualSensitive(TEXT("as left"), Read->Result.Value->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::Left));
	TestNull(TEXT("not held"), F.Held());
	TestEqual(TEXT("no heartbeat starts for it"), F.Provider->GetRepeatingCalls().GetCallCountForTesting(), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionGetLeftWhileHoldingTest, "AReadOfASessionThePlayerLeftIsNotReadAgainForTheHeldSessionsAnswers")
bool FFlockSessionGetLeftWhileHoldingTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.Server->Joins(Theirs, SessionPlayerA);
	F.Server->Leaves(Theirs, SessionPlayerA);
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Mine = F.Hosted(*this);
	if (Mine == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Read);
	const FSessionAnswerRef Read = F.Get(Theirs->Id);
	// The held session's heartbeat is sent and answered while the past session's read is on its way.
	F.Advance(20.0);
	TestEqual(TEXT("precondition: the held session heartbeated"), F.Server->Count(ERoute::Heartbeat), 1);
	Answer->Release();
	if (!TestTrue(TEXT("read"), Read->Succeeded() && Read->Result.Value != nullptr))
	{
		return false;
	}
	TestEqual(TEXT("read once: the held session's answers say nothing about it"), F.Server->Count(ERoute::Read), 1);
	TestEqualSensitive(TEXT("handed back ended as left"), Read->Result.Value->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::Left));
	TestTrue(TEXT("the held session is untouched"), F.Held() == Mine && !Mine->HasEnded());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHostAnotherTest, "HostingAnotherSessionEndsTheHeldOneAtOnceAsMovedToAnotherSession")
bool FFlockSessionHostAnotherTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* First = F.Hosted(*this);
	if (First == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(First);
	UFlockMultiplayerSession* Second = F.Hosted(*this);
	TestEqualSensitive(TEXT("the first ended once, at once"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::MovedToAnotherSession));
	TestTrue(TEXT("the second is held"), Second != nullptr && F.Held() == Second);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionJoinAnotherTest, "JoiningAnotherSessionEndsTheHeldOneAtOnceAsMovedToAnotherSession")
bool FFlockSessionJoinAnotherTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* First = F.Hosted(*this);
	if (First == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(First);
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	UFlockMultiplayerSession* Second = F.Joined(*this, Theirs->Code);
	TestEqualSensitive(TEXT("the first ended once, at once"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::MovedToAnotherSession));
	TestTrue(TEXT("the joined one is held"), Second != nullptr && F.Held() == Second);
	return true;
}

// ---- one writer: numbered readings and epochs ----

FLOCK_SESSION_TEST(FFlockSessionReadBeforeChangeTest, "AReadSentBeforeThisGamesChangeFinishedIsReadAgainAndChangesNothingBack")
bool FFlockSessionReadBeforeChangeTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> OldRead = F.Server->HoldNext(ERoute::Read);
	const FSessionAnswerRef Read = F.Get(Session->GetId());
	TestTrue(TEXT("the read is on its way, from before the publish"), OldRead->bArrived);
	TestTrue(TEXT("the publish finished"), F.Publish(Session, TEXT("direct"), FFlockCommandData().Set(TEXT("address"), TEXT("10.0.0.2")))->Succeeded());
	TestTrue(TEXT("precondition: the publish's answer shows the address"), Session->HasConnection());
	OldRead->Release();
	TestTrue(TEXT("the read answered"), Read->Succeeded());
	TestTrue(TEXT("the address is still there"), Session->HasConnection());
	TestEqual(TEXT("the overtaken read was sent again"), F.Server->Count(ERoute::Read), 2);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionOlderReadTest, "AnOlderReadLandingAfterANewerOneChangesNothing")
bool FFlockSessionOlderReadTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const TSharedRef<FFlockFakePartyServer::FHeld> OldRead = F.Server->HoldNext(ERoute::Read);
	F.Get(Session->GetId());
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Get(Session->GetId());
	TestEqualSensitive(TEXT("the newer read showed B"), PlayersOf(Session), FString(TEXT("player-a,player-b")));
	OldRead->Release();
	TestEqualSensitive(TEXT("the older read did not take B away"), PlayersOf(Session), FString(TEXT("player-a,player-b")));
	TestEqual(TEXT("players changed once"), Listener->PlayersChanged, 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionKeepsChangingTest, "AReadOvertakenEveryTimeGivesUpAndSaysWhy")
bool FFlockSessionKeepsChangingTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	// Every read the call sends is held, and overtaken by a change finishing before it lands.
	TSharedRef<FFlockFakePartyServer::FHeld> Pending = F.Server->HoldNext(ERoute::Read);
	const FSessionAnswerRef Read = F.Get(Session->GetId());
	for (int32 Round = 1; Round <= FFlockMultiplayerSessions::MostReadsWhileTheSessionChanges; ++Round)
	{
		TestTrue(FString::Printf(TEXT("round %d: the change finished"), Round),
			F.Publish(Session, TEXT("direct"), FFlockCommandData().Set(TEXT("round"), Round))->Succeeded());
		const TSharedRef<FFlockFakePartyServer::FHeld> Next = F.Server->HoldNext(ERoute::Read);
		Pending->Release();
		Pending = Next;
	}
	TestTrue(TEXT("the call gave up"), Read->bAnswered && !Read->Result.bSuccess);
	TestTrue(TEXT("it says the session kept changing"), Read->Result.Error.Message.Contains(TEXT("kept changing")));
	TestEqual(TEXT("it read as many times as it may"), F.Server->Count(ERoute::Read), FFlockMultiplayerSessions::MostReadsWhileTheSessionChanges);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionPublishOrderTest, "TwoPublishesAnsweredOutOfOrderKeepTheNewerConnection")
bool FFlockSessionPublishOrderTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> First = F.Server->HoldNext(ERoute::Publish);
	F.Publish(Session, TEXT("direct"), FFlockCommandData().Set(TEXT("address"), TEXT("first")));
	F.Publish(Session, TEXT("direct"), FFlockCommandData().Set(TEXT("address"), TEXT("second")));
	First->Release();
	FString Address;
	Session->GetConnection().Values.TryGetString(TEXT("address"), Address);
	TestEqualSensitive(TEXT("the newer publish stays"), Address, FString(TEXT("second")));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionOlderChangeAnswerTest, "AChangeAnswerOlderThanAReadAlreadyTakenChangesNothing")
bool FFlockSessionOlderChangeAnswerTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Publish);
	F.Publish(Session, TEXT("direct"), FFlockCommandData().Set(TEXT("address"), TEXT("first")));
	// The host publishes again from another device, and this game reads it before the first answer lands.
	F.Server->Publishes(F.Server->Sessions[0], DirectConnection(TEXT("second")));
	F.Get(Session->GetId());
	Answer->Release();
	FString Address;
	Session->GetConnection().Values.TryGetString(TEXT("address"), Address);
	TestEqualSensitive(TEXT("the reading with the higher connection epoch stays"), Address, FString(TEXT("second")));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionOlderHostAnswerTest, "AChangeAnswerWithAnOlderHostEpochChangesNothing")
bool FFlockSessionOlderHostAnswerTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Publish);
	F.Publish(Session, TEXT("direct"));
	TestTrue(TEXT("A's publish is on its way, its answer showing A hosting"), Answer->bArrived);
	// Hosting moves to B on the server (the connection epoch stays), and this game reads it before the publish's answer lands.
	F.Server->MakesHost(F.Server->Sessions[0], SessionPlayerB);
	F.Get(Session->GetId());
	TestEqualSensitive(TEXT("precondition: B hosts"), Session->GetHostPlayerId(), SessionPlayerB);
	Answer->Release();
	TestEqualSensitive(TEXT("the older answer did not put A back as host"), Session->GetHostPlayerId(), SessionPlayerB);
	TestFalse(TEXT("nor the address the hand-over cleared"), Session->HasConnection());
	return true;
}

// ---- events ----

FLOCK_SESSION_TEST(FFlockSessionEventsTest, "EventsSeeTheSessionAlreadyUpdated")
bool FFlockSessionEventsTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("published"), F.Publish(Session, TEXT("direct"), FFlockCommandData().Set(TEXT("address"), TEXT("10.0.0.2")))->Succeeded());
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Server->MakesHost(F.Server->Sessions[0], SessionPlayerB);
	TestTrue(TEXT("read"), F.Get(Session->GetId())->Succeeded());
	TestTrue(TEXT("players changed, seen with two"), Listener->PlayersSeen.Num() == 1 && Listener->PlayersSeen[0] == 2);
	TestTrue(TEXT("host changed, seen with B hosting"), Listener->HostsSeen.Num() == 1 && Listener->HostsSeen[0] == SessionPlayerB);
	TestTrue(TEXT("connection changed, seen cleared"), Listener->ConnectionSeen.Num() == 1 && !Listener->ConnectionSeen[0]);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionEventsAfterHeartbeatTest, "EventsSeeTheHeartbeatAlreadyFollowingTheReading")
bool FFlockSessionEventsAfterHeartbeatTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	double IntervalSeen = 0.0;
	FFlockMultiplayerSessions& Sessions = F.Sessions();
	Listener->AlsoInHandler = [&Sessions, &IntervalSeen]()
	{
		IntervalSeen = Sessions.GetHeartbeatForTesting().IsValid() ? Sessions.GetHeartbeatForTesting()->GetIntervalSeconds() : 0.0;
	};
	// One reading brings a new player and a new interval together.
	F.Server->HeartbeatSeconds = 8;
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Get(Session->GetId());
	TestEqual(TEXT("players changed"), Listener->PlayersChanged, 1);
	TestEqual(TEXT("inside the handler the heartbeat already follows the reading's interval"), IntervalSeen, 8.0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionPlayerReplacedTest, "APlayerReplacedByAnotherRaisesPlayersChanged")
bool FFlockSessionPlayerReplacedTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Get(Session->GetId());
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->Leaves(F.Server->Sessions[0], SessionPlayerB);
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerC);
	F.Get(Session->GetId());
	TestEqualSensitive(TEXT("C is seen in B's place"), PlayersOf(Session), FString(TEXT("player-a,player-c")));
	TestEqual(TEXT("players changed, though the count did not"), Listener->PlayersChanged, 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHostEpochTest, "HostChangedFollowsTheHostEpochEvenWhenHostingCameBack")
bool FFlockSessionHostEpochTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Server->MakesHost(F.Server->Sessions[0], SessionPlayerB);
	F.Server->MakesHost(F.Server->Sessions[0], SessionPlayerA);
	F.Get(Session->GetId());
	TestEqual(TEXT("host changed once, though the same player hosts"), Listener->HostChanged, 1);
	TestTrue(TEXT("the player hosts again"), Session->IsHost());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionConnectionEpochTest, "ConnectionChangedFollowsTheConnectionEpochEvenWithTheSameValues")
bool FFlockSessionConnectionEpochTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const FFlockCommandData Values = FFlockCommandData().Set(TEXT("address"), TEXT("10.0.0.2"));
	F.Publish(Session, TEXT("direct"), Values);
	F.Publish(Session, TEXT("direct"), Values);
	TestEqual(TEXT("raised for each publish"), Listener->ConnectionChanged, 2);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionQuietReadTest, "AReadingThatChangesNothingRaisesNothing")
bool FFlockSessionQuietReadTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Get(Session->GetId());
	F.Advance(20.0);
	TestEqual(TEXT("precondition: a heartbeat was taken too"), F.Server->Count(ERoute::Heartbeat), 1);
	TestEqual(TEXT("nothing raised"), Listener->PlayersChanged + Listener->HostChanged + Listener->ConnectionChanged + Listener->Ended.Num(), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHostChangeClearsTest, "AHostChangeClearsTheConnectionAndRaisesConnectionChanged")
bool FFlockSessionHostChangeClearsTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.Server->Publishes(Theirs, DirectConnection(TEXT("10.0.0.9")));
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Joined(*this, Theirs->Code);
	if (Session == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("precondition: the player sees the host's address"), Session->HasConnection());
	TestEqualSensitive(TEXT("its mode"), Session->GetConnection().Mode, FString(TEXT("direct")));
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->Leaves(Theirs, SessionPlayerB);
	F.Get(Session->GetId());
	TestTrue(TEXT("the player hosts now"), Session->IsHost());
	TestFalse(TEXT("the old host's address is gone"), Session->HasConnection());
	TestTrue(TEXT("and the connection's mode with it"), Session->GetConnection().Mode.IsEmpty());
	TestEqual(TEXT("connection changed for it"), Listener->ConnectionChanged, 1);
	return true;
}

// ---- calls on a session ----

FLOCK_SESSION_TEST(FFlockSessionPublishTest, "PublishConnectionSendsTheModeWithItsValuesAndRefusesWhatIsNotAllowed")
bool FFlockSessionPublishTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FSessionAnswerRef Published = F.Publish(Session, TEXT("direct"),
		FFlockCommandData().Set(TEXT("Address"), TEXT("10.0.0.2")).Set(TEXT("port"), 7777).Set(TEXT("mode"), TEXT("ignored")));
	TestTrue(TEXT("published"), Published->Succeeded());
	const TSharedPtr<FJsonObject> Sent = SessionBodyOf(F.Server->RequestsTo(ERoute::Publish)[0]);
	const TSharedPtr<FJsonObject>* Connection = nullptr;
	if (TestTrue(TEXT("connection_info was sent"), Sent.IsValid() && Sent->TryGetObjectField(TEXT("connection_info"), Connection)))
	{
		FString Mode;
		(*Connection)->TryGetStringField(TEXT("mode"), Mode);
		TestEqualSensitive(TEXT("the mode given goes with the values"), Mode, FString(TEXT("direct")));
		TestTrue(TEXT("a value's key as written"), FlockTestSpelling::HasMemberSpelled(*Connection, TEXT("Address")));
	}
	TestTrue(TEXT("the session has the connection"), Session->HasConnection());
	TestEqualSensitive(TEXT("its mode"), Session->GetConnection().Mode, FString(TEXT("direct")));
	FString Address;
	TestTrue(TEXT("its values"), Session->GetConnection().Values.TryGetString(TEXT("Address"), Address) && Address == TEXT("10.0.0.2"));

	const int32 Sent0 = F.Server->Count(ERoute::Publish);
	TestEqual(TEXT("an empty mode is refused"), F.Publish(Session, FString())->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("and sends nothing"), F.Server->Count(ERoute::Publish), Sent0);
	const FSessionAnswerRef TooLong = F.Publish(Session, FString::ChrN(33, TEXT('m')));
	TestEqual(TEXT("a mode over 32 characters is the server's refusal"), TooLong->FailureType(), EFlockErrorType::Validation);
	TestFalse(TEXT("which ends nothing"), Session->HasEnded());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionNotHostTest, "ANotHostRefusalEndsNothingAndReadsNothing")
bool FFlockSessionNotHostTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Joined(*this, Theirs->Code);
	if (Session == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("publish refused on its code"), F.Publish(Session, TEXT("direct"))->FailedWith(EFlockErrorCode::MultiplayerNotHost));
	TestTrue(TEXT("end refused on its code"), F.End(Session)->FailedWith(EFlockErrorCode::MultiplayerNotHost));
	TestTrue(TEXT("make host refused on its code"), F.MakeHost(Session, SessionPlayerB)->FailedWith(EFlockErrorCode::MultiplayerNotHost));
	TestFalse(TEXT("nothing ended"), Session->HasEnded());
	TestEqual(TEXT("not read to learn why: the player is still seated"), F.Server->Count(ERoute::Read), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionMakeHostTest, "MakeHostHandsHostingOverAndTheServersRefusalsKeepTheirCodes")
bool FFlockSessionMakeHostTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	TestTrue(TEXT("a player not in the session is refused on its code"),
		F.MakeHost(Session, SessionPlayerD)->FailedWith(EFlockErrorCode::MultiplayerTargetNotAParticipant));
	TestEqual(TEXT("an empty player id is refused"), F.MakeHost(Session, FString())->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("both sent at most the server's one"), F.Server->Count(ERoute::MakeHost), 1);
	F.Publish(Session, TEXT("direct"));
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	TestTrue(TEXT("handed over"), F.MakeHost(Session, SessionPlayerB)->Succeeded());
	TestFalse(TEXT("the player no longer hosts"), Session->IsHost());
	TestFalse(TEXT("the old address is cleared"), Session->HasConnection());
	TestEqual(TEXT("host changed"), Listener->HostChanged, 1);
	TestEqual(TEXT("connection changed"), Listener->ConnectionChanged, 1);
	TestTrue(TEXT("a second hand-over is refused on its code"), F.MakeHost(Session, SessionPlayerB)->FailedWith(EFlockErrorCode::MultiplayerNotHost));
	TestFalse(TEXT("nothing ended"), Session->HasEnded());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionJoinTokensTest, "JoinTokensAMemberAsksTheHostVerifiesAndAWrongOneIsRefused")
bool FFlockSessionJoinTokensTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	const TSharedRef<FTokenAnswer> Token = MakeShared<FTokenAnswer>();
	Session->RequestJoinToken(Into(Token));
	TestTrue(TEXT("a token"), Token->Succeeded() && !Token->Result.Value.Token.IsEmpty());
	TestEqual(TEXT("good for 60 s"), Token->Result.Value.ExpiresInSeconds, 60);

	const TSharedRef<FPlayerAnswer> Verified = MakeShared<FPlayerAnswer>();
	Session->VerifyJoinToken(FString::Printf(TEXT("token-%s-%s"), *Session->GetId(), *SessionPlayerB), Into(Verified));
	TestTrue(TEXT("the host verifies B's token"), Verified->Succeeded());
	TestEqualSensitive(TEXT("and learns it is B"), Verified->Result.Value, SessionPlayerB);

	const TSharedRef<FPlayerAnswer> Wrong = MakeShared<FPlayerAnswer>();
	Session->VerifyJoinToken(TEXT("token-made-up"), Into(Wrong));
	TestTrue(TEXT("a wrong token is refused on its code"), Wrong->FailedWith(EFlockErrorCode::MultiplayerInvalidJoinToken));
	const TSharedRef<FPlayerAnswer> Empty = MakeShared<FPlayerAnswer>();
	const int32 Before = F.Server->Count(ERoute::Verify);
	Session->VerifyJoinToken(FString(), Into(Empty));
	TestEqual(TEXT("an empty token is refused"), Empty->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("without a request"), F.Server->Count(ERoute::Verify), Before);

	TestTrue(TEXT("handed over"), F.MakeHost(Session, SessionPlayerB)->Succeeded());
	const TSharedRef<FPlayerAnswer> NotHost = MakeShared<FPlayerAnswer>();
	Session->VerifyJoinToken(TEXT("token-anything"), Into(NotHost));
	TestTrue(TEXT("a player who no longer hosts cannot verify"), NotHost->FailedWith(EFlockErrorCode::MultiplayerNotHost));
	TestFalse(TEXT("nothing ended"), Session->HasEnded());
	return true;
}

// ---- ending ----

FLOCK_SESSION_TEST(FFlockSessionLeaveTest, "LeavingEndsAsLeftOnceAndLaterCallsAreRefusedWithoutARequest")
bool FFlockSessionLeaveTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	TestTrue(TEXT("left"), F.Leave(Session)->Succeeded());
	TestEqualSensitive(TEXT("ended once, as left"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Left));
	TestTrue(TEXT("the handler found it over"), Listener->EndedSawSessionOver.Num() == 1 && Listener->EndedSawSessionOver[0]);
	TestNull(TEXT("nothing held"), F.Held());
	const int32 Sent = F.Server->Requests.Num();
	TestEqual(TEXT("a publish afterwards is refused"), F.Publish(Session, TEXT("direct"))->FailureType(), EFlockErrorType::Validation);
	const TSharedRef<FTokenAnswer> Token = MakeShared<FTokenAnswer>();
	Session->RequestJoinToken(Into(Token));
	TestEqual(TEXT("a token afterwards is refused"), Token->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("a second leave is refused"), F.Leave(Session)->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("and none of them sent anything"), F.Server->Requests.Num(), Sent);
	TestTrue(TEXT("the server passed hosting on"), F.Server->Sessions[0]->HostId == SessionPlayerB);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionLeaveAlreadyOutTest, "LeavingWhenTheServerSaysThePlayerIsAlreadyOutStillEndsAsLeft")
bool FFlockSessionLeaveAlreadyOutTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Server->Leaves(F.Server->Sessions[0], SessionPlayerA);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const FSessionAnswerRef Left = F.Leave(Session);
	TestTrue(TEXT("the leave counts"), Left->Succeeded());
	TestEqualSensitive(TEXT("ended as left"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Left));
	TestEqual(TEXT("not logged as a failed call (the HTTP trace notes every 4xx as a warning)"), F.Log->Errors.Num(), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionLeaveRefusedTest, "ALeaveTheServerRefusesForAnotherReasonKeepsTheSession")
bool FFlockSessionLeaveRefusedTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::Leave, FFlockFakeSessionServer::Refused(400, TEXT("request.validation_failed")));
	TestFalse(TEXT("the leave failed"), F.Leave(Session)->Succeeded());
	TestFalse(TEXT("the session carries on"), Session->HasEnded());
	// The leave's own flag is put back: a later reading that finds the player out is not taken for this game's leave.
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Server->Drops(F.Server->Sessions[0], SessionPlayerA);
	F.Get(Session->GetId());
	TestEqualSensitive(TEXT("a later drop reads as dropped"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::Dropped));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionLeaveOfAnotherTest, "ALeaveOnItsWayForOneSessionLeavesAnotherSessionsEndAsTheServerGivesIt")
bool FFlockSessionLeaveOfAnotherTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* First = F.Hosted(*this);
	if (First == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Leave);
	F.Leave(First);
	TestTrue(TEXT("precondition: the leave of the first is on its way"), Answer->bArrived && !Answer->bDelivered);
	UFlockMultiplayerSession* Second = F.Hosted(*this);
	if (Second == nullptr)
	{
		return false;
	}
	// The second session expires on the server while the first's leave is still on its way, and this game reads it.
	F.Server->Ends(F.Server->Sessions[1], TEXT("expired"));
	F.Get(Second->GetId());
	TestEqualSensitive(TEXT("the second ends with the server's reason, not as the first's leave"), Second->GetEndReason(), FString(TEXT("expired")));
	Answer->Release();
	TestEqualSensitive(TEXT("the first stays as it ended"), First->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::MovedToAnotherSession));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionEarlierLeaveAnsweredTest, "ALeaveAnsweredForAnEarlierSessionKeepsTheNewerLeaveThisGames")
bool FFlockSessionEarlierLeaveAnsweredTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* First = F.Hosted(*this);
	if (First == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> FirstLeave = F.Server->HoldNext(ERoute::Leave);
	F.Leave(First);
	UFlockMultiplayerSession* Second = F.Hosted(*this);
	if (Second == nullptr)
	{
		return false;
	}
	// The second is left too, alone, which closes it as host_left on the server.
	const TSharedRef<FFlockFakePartyServer::FHeld> SecondLeave = F.Server->HoldNext(ERoute::Leave);
	F.Leave(Second);
	TestTrue(TEXT("precondition: both leaves are on their way"), FirstLeave->bArrived && SecondLeave->bArrived && !SecondLeave->bDelivered);
	FirstLeave->Release();
	F.Get(Second->GetId());
	TestEqualSensitive(TEXT("the first leave's answer did not forget the second: the game's own leave is left"), Second->GetEndReason(),
		FString(FlockMultiplayerSessionEndReasons::Left));
	SecondLeave->Release();
	TestEqualSensitive(TEXT("and stays so"), Second->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::Left));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionReadBeforeLeavingTest, "AReadSentBeforeLeavingDoesNotBringTheSessionBack")
bool FFlockSessionReadBeforeLeavingTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Read);
	const FSessionAnswerRef Read = F.Get(Session->GetId());
	TestTrue(TEXT("precondition: the read was answered while A was seated"), Answer->bArrived && !Answer->bDelivered);
	TestTrue(TEXT("left"), F.Leave(Session)->Succeeded());
	Answer->Release();
	if (!TestTrue(TEXT("the read finished"), Read->Succeeded() && Read->Result.Value != nullptr))
	{
		return false;
	}
	TestNull(TEXT("the older read did not take the session back up"), F.Held());
	TestTrue(TEXT("what it handed back has ended"), Read->Result.Value->HasEnded());
	TestFalse(TEXT("no heartbeat runs for it"), F.Sessions().GetHeartbeatForTesting().IsValid());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHostLeavesAloneTest, "TheHostLeavingAloneEndsAsLeftNotAsHostLeft")
bool FFlockSessionHostLeavesAloneTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	TestTrue(TEXT("left"), F.Leave(Session)->Succeeded());
	TestEqualSensitive(TEXT("precondition: the server closed it as host_left"), F.Server->Sessions[0]->EndedReason, FString(TEXT("host_left")));
	TestEqualSensitive(TEXT("the game's own leave is left"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Left));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionEndTest, "EndingEndsAsEndedByHost")
bool FFlockSessionEndTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	TestTrue(TEXT("ended"), F.End(Session)->Succeeded());
	TestEqualSensitive(TEXT("as ended by host, once"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::EndedByHost));
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionOverTest, "ACallThatFindsTheSessionOverReadsWhyAndEndsWithTheServersReason")
bool FFlockSessionOverTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Joined(*this, Theirs->Code);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->Ends(Theirs, TEXT("expired"));
	const TSharedRef<FTokenAnswer> Token = MakeShared<FTokenAnswer>();
	Session->RequestJoinToken(Into(Token));
	TestTrue(TEXT("the call fails on its code"), Token->FailedWith(EFlockErrorCode::MultiplayerSessionNotFound));
	TestEqual(TEXT("the session was read once to learn why"), F.Server->Count(ERoute::Read), 1);
	TestEqualSensitive(TEXT("ended with the server's reason, before the call was answered"), Listener->EndedAs(), FString(TEXT("expired")));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionDroppedTest, "ACallThatFindsThePlayerDroppedEndsAsDropped")
bool FFlockSessionDroppedTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Server->Drops(F.Server->Sessions[0], SessionPlayerA);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	TestTrue(TEXT("the publish fails on its code"), F.Publish(Session, TEXT("direct"))->FailedWith(EFlockErrorCode::MultiplayerSessionNotFound));
	TestEqualSensitive(TEXT("ended as dropped"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Dropped));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionUnreadableTest, "ACallWhoseSessionCannotBeReadAgainEndsAsDropped")
bool FFlockSessionUnreadableTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->AnswerNext(ERoute::JoinToken, FFlockFakeSessionServer::Refused(404, TEXT("multiplayer.session_not_found")));
	F.Server->AnswerNext(ERoute::Read, FFlockFakeSessionServer::Refused(404, TEXT("multiplayer.session_not_found")));
	const TSharedRef<FTokenAnswer> Token = MakeShared<FTokenAnswer>();
	Session->RequestJoinToken(Into(Token));
	TestTrue(TEXT("the call fails on its code"), Token->FailedWith(EFlockErrorCode::MultiplayerSessionNotFound));
	TestEqualSensitive(TEXT("ended as dropped"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Dropped));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionEndedHandlerTest, "AnEndHandlerThatCallsBackInFindsTheSessionOver")
bool FFlockSessionEndedHandlerTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	UFlockMultiplayerSession* HeldInHandler = Session;
	FSessionAnswerRef LeaveInHandler = NewAnswer();
	Listener->AlsoInHandler = [&]()
	{
		HeldInHandler = F.Held();
		Session->Leave(Into(LeaveInHandler));
	};
	F.End(Session);
	TestEqualSensitive(TEXT("ended once"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::EndedByHost));
	TestTrue(TEXT("the handler found it over"), Listener->EndedSawSessionOver.Num() == 1 && Listener->EndedSawSessionOver[0]);
	TestNull(TEXT("the handler found nothing held"), HeldInHandler);
	TestEqual(TEXT("a call from the handler is refused"), LeaveInHandler->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("and sends nothing"), F.Server->Count(ERoute::Leave), 0);
	return true;
}

// ---- sign-in ----

FLOCK_SESSION_TEST(FFlockSessionSignOutTest, "SignOutEndsTheSessionAtOnceRaisesEndedOnTheNextFrameAndRefusesItsCalls")
bool FFlockSessionSignOutTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.SignOut();
	TestTrue(TEXT("ended in the same frame as the sign-out"), Session->HasEnded());
	TestEqualSensitive(TEXT("as signed out"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	TestEqual(TEXT("not raised before the next frame"), Listener->Ended.Num(), 0);
	TestEqual(TEXT("a call on it is refused"), F.Publish(Session, TEXT("direct"))->FailureType(), EFlockErrorType::Validation);
	F.Frame();
	TestEqualSensitive(TEXT("raised once on the next frame"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	F.Advance(20.0);
	F.Advance(20.0);
	TestEqualSensitive(TEXT("still once"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	TestEqual(TEXT("no heartbeat after the sign-out"), F.Server->Count(ERoute::Heartbeat), 0);
	TestEqual(TEXT("the refused publish sent nothing"), F.Server->Count(ERoute::Publish), 0);
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionSwitchThenMineTest, "APlayerSwitchThenGettingMySessionEndsTheOldPlayersSessionAsSignedOut")
bool FFlockSessionSwitchThenMineTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	// PlayerB signs in with no sign-out between, and asks in the same frame, before the frame check runs.
	F.SignIn(SessionPlayerB);
	const FSessionAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("B is seated nowhere"), Mine->Succeeded() && Mine->Result.Value == nullptr);
	TestEqualSensitive(TEXT("A's session ended as signed out"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	TestEqual(TEXT("and was not read for B"), F.Server->Count(ERoute::Read), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionLeaveAfterSwitchTest, "ALeaveAnsweredAfterTheSignInEndedEndsTheSessionAsSignedOut")
bool FFlockSessionLeaveAfterSwitchTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Leave);
	F.Leave(Session);
	TestTrue(TEXT("A's leave is on its way"), Answer->bArrived);
	// PlayerB signs in, and PlayerA's leave lands before the frame check runs.
	F.SignIn(SessionPlayerB);
	Answer->Release();
	F.Frame();
	TestEqualSensitive(TEXT("ended once, with the reason it already read as"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	TestEqualSensitive(TEXT("signed out"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionAlreadyOutAfterSwitchTest, "ALeaveRefusedAsAlreadyOutAfterTheSignInEndedEndsTheSessionAsSignedOut")
bool FFlockSessionAlreadyOutAfterSwitchTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Server->Leaves(F.Server->Sessions[0], SessionPlayerA);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Leave);
	F.Leave(Session);
	TestTrue(TEXT("A's leave is on its way, to be answered \"not a participant\""), Answer->bArrived);
	F.SignIn(SessionPlayerC);
	Answer->Release();
	F.Frame();
	TestEqualSensitive(TEXT("ended once, as signed out"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionSwitchSameSessionTest, "APlayerSwitchThenReadingTheSameSessionGivesTheNewPlayerItsOwnObject")
bool FFlockSessionSwitchSameSessionTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* OfA = F.Hosted(*this);
	if (OfA == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	UFlockSessionTestListener* Listener = F.Listen(OfA);
	// PlayerB signs in with no sign-out between, and reads the same session in the same frame, before the frame check runs.
	F.SignIn(SessionPlayerB);
	const FSessionAnswerRef SeenByB = F.Get(OfA->GetId());
	UFlockMultiplayerSession* OfB = SeenByB->Result.Value;
	if (!TestTrue(TEXT("B read it"), SeenByB->Succeeded() && OfB != nullptr))
	{
		return false;
	}
	TestTrue(TEXT("B gets an object of its own"), OfB != OfA);
	TestFalse(TEXT("which knows B does not host"), OfB->IsHost());
	TestEqualSensitive(TEXT("A's ended as signed out"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	TestTrue(TEXT("B's is held"), F.Held() == OfB);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionLateChangeReadTest, "ALateChangeAnswerFromTheLastSignInDoesNotSendTheNewPlayersReadAgain")
bool FFlockSessionLateChangeReadTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Publish = F.Server->HoldNext(ERoute::Publish);
	F.Publish(Session, TEXT("direct"));
	TestTrue(TEXT("A's publish is on its way"), Publish->bArrived);

	F.SignIn(SessionPlayerD);
	F.Server->HostedBy(SessionPlayerD);
	const TSharedRef<FFlockFakePartyServer::FHeld> Read = F.Server->HoldNext(ERoute::Current);
	const FSessionAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("D's read is on its way"), Read->bArrived);
	Publish->Release();
	Read->Release();
	TestTrue(TEXT("D's read was taken"), Mine->Succeeded() && Mine->Result.Value != nullptr);
	TestEqual(TEXT("A's answer did not send D's read again"), F.Server->Count(ERoute::Current), 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionLateChangeAloneTest, "ALateChangeAnswerFromTheLastSignInLeavesTheNewPlayersSessionAlone")
bool FFlockSessionLateChangeAloneTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* OfA = F.Hosted(*this);
	if (OfA == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Publish = F.Server->HoldNext(ERoute::Publish);
	F.Publish(OfA, TEXT("direct"));
	TestTrue(TEXT("A's publish is on its way, its answer showing A and B"), Publish->bArrived);

	// B signs in on this device; C joins the same session, and B reads it.
	F.SignIn(SessionPlayerB);
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerC);
	const FSessionAnswerRef SeenByB = F.Get(OfA->GetId());
	UFlockMultiplayerSession* OfB = SeenByB->Result.Value;
	if (!TestTrue(TEXT("B read the session"), SeenByB->Succeeded() && OfB != nullptr))
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(OfB);
	Publish->Release();
	TestEqualSensitive(TEXT("an answer A asked for changes nothing for B"), PlayersOf(OfB), FString(TEXT("player-a,player-b,player-c")));
	TestEqual(TEXT("nothing raised on B's"), Listener->PlayersChanged + Listener->HostChanged + Listener->ConnectionChanged + Listener->Ended.Num(), 0);
	TestTrue(TEXT("B's session is held"), F.Held() == OfB);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionSignInEndedOnTheWayTest, "ACallWhoseSignInEndedOnTheWayIsCancelledAndHoldsNothing")
bool FFlockSessionSignInEndedOnTheWayTest::RunTest(const FString& Parameters)
{
	{
		FSessionsFixture F;
		F.SignIn(SessionPlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Host);
		const FSessionAnswerRef Hosted = F.Host();
		F.SignOut();
		Answer->Release();
		TestEqual(TEXT("host: cancelled"), Hosted->FailureType(), EFlockErrorType::Cancelled);
		TestNull(TEXT("host: nothing held"), F.Held());
	}
	{
		FSessionsFixture F;
		const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
		F.SignIn(SessionPlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Join);
		const FSessionAnswerRef Joined = F.Join(Theirs->Code);
		F.SignIn(SessionPlayerC);
		Answer->Release();
		TestEqual(TEXT("join: cancelled"), Joined->FailureType(), EFlockErrorType::Cancelled);
		TestNull(TEXT("join: nothing held"), F.Held());
	}
	{
		FSessionsFixture F;
		const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
		F.Server->Joins(Theirs, SessionPlayerA);
		F.SignIn(SessionPlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Read);
		const FSessionAnswerRef Read = F.Get(Theirs->Id);
		F.SignOut();
		Answer->Release();
		TestEqual(TEXT("get: cancelled"), Read->FailureType(), EFlockErrorType::Cancelled);
		TestNull(TEXT("get: nothing held"), F.Held());
	}
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionRetryAfterSwitchTest, "ARetryAfterAPlayerSwitchIsNotSentAsTheNewPlayer")
bool FFlockSessionRetryAfterSwitchTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F(/*MaxRetries*/ 1);
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.SignIn(SessionPlayerA);
	F.Server->AnswerNext(ERoute::Join, FFlockFakeSessionServer::Refused(429, TEXT("request.rate_limited")));
	const TSharedRef<FFlockFakePartyServer::FHeld> FirstTry = F.Server->HoldNext(ERoute::Join);
	const FSessionAnswerRef Joined = F.Join(Theirs->Code);
	TestTrue(TEXT("the join's first try is on its way"), FirstTry->bArrived);
	F.SignIn(SessionPlayerD);
	FirstTry->Release();
	FSessionsFixture::PumpRetries();
	TestEqual(TEXT("a request whose sign-in ended is cancelled"), Joined->FailureType(), EFlockErrorType::Cancelled);
	TestEqual(TEXT("the retry did not go out as the new player"), F.Server->Count(ERoute::Join), 1);
	TestFalse(TEXT("D was never seated"), Theirs->IsSeated(SessionPlayerD));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionRetryControlTest, "AJoinRefusedForALimitIsTriedAgainForTheSameSignIn")
bool FFlockSessionRetryControlTest::RunTest(const FString& Parameters)
{
	// The control for the switch test above: with no switch the same 429 is tried again.
	FSessionsFixture F(/*MaxRetries*/ 1);
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.SignIn(SessionPlayerA);
	F.Server->AnswerNext(ERoute::Join, FFlockFakeSessionServer::Refused(429, TEXT("request.rate_limited")));
	const FSessionAnswerRef Joined = F.Join(Theirs->Code);
	FSessionsFixture::PumpRetries();
	TestTrue(TEXT("joined on the second try"), Joined->Succeeded());
	TestEqual(TEXT("sent twice"), F.Server->Count(ERoute::Join), 2);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionSignedOutTest, "SignedOutAndEmptyInputsAreRefusedWithoutARequest")
bool FFlockSessionSignedOutTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FSessionAnswerRef SignedOut = F.Host();
	TestEqual(TEXT("host refused"), SignedOut->FailureType(), EFlockErrorType::Auth);
	TestTrue(TEXT("saying nobody is signed in"), SignedOut->Result.Error.Message.Contains(TEXT("No player is signed in")));
	TestEqual(TEXT("join refused"), F.Join(TEXT("S10001"))->FailureType(), EFlockErrorType::Auth);
	TestEqual(TEXT("get refused"), F.Get(TEXT("01SESSION"))->FailureType(), EFlockErrorType::Auth);
	TestEqual(TEXT("get mine refused"), F.GetMine()->FailureType(), EFlockErrorType::Auth);
	F.SignIn(SessionPlayerA);
	TestEqual(TEXT("an empty join code is refused"), F.Join(FString())->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("an empty session id is refused"), F.Get(FString())->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("no request"), F.Server->Requests.Num(), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionNoPlayerIdTest, "ASignInThatNamesNoPlayerIsRefusedWithoutARequest")
bool FFlockSessionNoPlayerIdTest::RunTest(const FString& Parameters)
{
	// A call's reading is told from a heartbeat's by the player id, so a sign-in whose token names no player must send nothing.
	FSessionsFixture F;
	F.SignIn(FString());
	if (!TestTrue(TEXT("precondition: signed in with a token that names no player"), F.Session->IsAuthenticated()
		&& F.Session->GetPlayerId().IsEmpty()))
	{
		return false;
	}
	const FSessionAnswerRef Hosted = F.Host();
	TestEqual(TEXT("host refused"), Hosted->FailureType(), EFlockErrorType::Auth);
	TestTrue(TEXT("saying the sign-in names no player"), Hosted->Result.Error.Message.Contains(TEXT("names no player")));
	TestEqual(TEXT("get mine refused"), F.GetMine()->FailureType(), EFlockErrorType::Auth);
	TestEqual(TEXT("no request"), F.Server->Requests.Num(), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionTokenAfterRefreshTest, "ACallRefusedAsSignedOutIsSentAgainWithTheRefreshedToken")
bool FFlockSessionTokenAfterRefreshTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const FString Refreshed = SessionJwt(SessionPlayerA, 7);
	const TSharedRef<FJsonObject> TokenReply = MakeShared<FJsonObject>();
	TokenReply->SetStringField(TEXT("player_id"), SessionPlayerA);
	TokenReply->SetStringField(TEXT("access_token"), Refreshed);
	TokenReply->SetStringField(TEXT("refresh_token"), TEXT("r-2"));
	F.Server->AnswerNext(ERoute::Publish, FFlockFakeSessionServer::Refused(401, TEXT("player.invalid_access_token")));
	F.Server->AnswerNext(ERoute::Other, FFlockFakePartyServer::Answer(200, TokenReply));
	TestTrue(TEXT("the publish went through after the refresh"), F.Publish(Session, TEXT("direct"))->Succeeded());
	const TArray<FFlockHttpRequest> Publishes = F.Server->RequestsTo(ERoute::Publish);
	if (TestEqual(TEXT("sent twice"), Publishes.Num(), 2))
	{
		TestEqualSensitive(TEXT("the replay carries the refreshed token"), Publishes[1].Headers.FindRef(TEXT("Authorization")), TEXT("Bearer ") + Refreshed);
	}
	TestFalse(TEXT("the sign-in carried on"), Session->HasEnded());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionRetriesTest, "ChangesALostAnswerCouldRepeatAreNeverSentTwice")
bool FFlockSessionRetriesTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F(/*MaxRetries*/ 1);
	F.SignIn(SessionPlayerA);
	auto Tries = [&F](ERoute Route, TFunction<void()> Call)
	{
		const int32 Before = F.Server->Count(Route);
		F.Server->AnswerNext(Route, FFlockFakeSessionServer::Refused(500, TEXT("server.error")));
		Call();
		FSessionsFixture::PumpRetries();
		return F.Server->Count(Route) - Before;
	};
	TestEqual(TEXT("host: once"), Tries(ERoute::Host, [&]() { F.Host(); }), 1);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions.Last(), SessionPlayerB);
	TestEqual(TEXT("end: once"), Tries(ERoute::End, [&]() { F.End(Session); }), 1);
	TestEqual(TEXT("make host: once"), Tries(ERoute::MakeHost, [&]() { F.MakeHost(Session, SessionPlayerB); }), 1);
	TestFalse(TEXT("none of those ended the session"), Session->HasEnded());
	// Controls: the calls a repeat cannot hurt are tried again.
	TestEqual(TEXT("publish: again"), Tries(ERoute::Publish, [&]() { F.Publish(Session, TEXT("direct")); }), 2);
	TestEqual(TEXT("join token: again"), Tries(ERoute::JoinToken, [&]() { Session->RequestJoinToken([](TFlockResult<FFlockMultiplayerSessionJoinToken>) {}); }), 2);
	TestEqual(TEXT("join: again"), Tries(ERoute::Join, [&]() { F.Join(Session->GetJoinCode()); }), 2);
	TestEqual(TEXT("leave: again"), Tries(ERoute::Leave, [&]() { F.Leave(Session); }), 2);
	TestEqualSensitive(TEXT("the leave went through on its second try"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::Left));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionCancelTest, "CancellingACallStopsTheRequestOnItsWayAndSendsNoOther")
bool FFlockSessionCancelTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Read);
	const FSessionAnswerRef Read = NewAnswer();
	FFlockRequestHandle Handle = F.Provider->GetSession(Session->GetId(), Into(Read));
	TestTrue(TEXT("the read is on its way"), Answer->bArrived);
	Handle.Cancel();
	TestEqual(TEXT("a read cancelled on its way"), Read->FailureType(), EFlockErrorType::Cancelled);
	TestEqual(TEXT("no other request after it"), F.Server->Count(ERoute::Read), 1);
	TestFalse(TEXT("the session carries on"), Session->HasEnded());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionNoSessionAnswerTest, "AnAnswerThatNamesNoSessionIsAFailureAndHoldsNothing")
bool FFlockSessionNoSessionAnswerTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	const TSharedRef<FJsonObject> Empty = MakeShared<FJsonObject>();
	Empty->SetStringField(TEXT("id"), FString());
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetObjectField(TEXT("result"), Empty);
	F.Server->AnswerNext(ERoute::Host, FFlockFakePartyServer::Answer(200, Body));
	const FSessionAnswerRef Hosted = F.Host();
	TestTrue(TEXT("a failure"), Hosted->bAnswered && !Hosted->Result.bSuccess);
	TestTrue(TEXT("saying the answer names no session"), Hosted->Result.Error.Message.Contains(TEXT("names no session")));
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionReadingTest, "AReadingTakesTheConnectionOnlyWithAModeAndRefusesASeatWithNoPlayer")
bool FFlockSessionReadingTest::RunTest(const FString& Parameters)
{
	auto Parse = [](const FString& Json, FFlockMultiplayerSessionReading& Out, FString& Error)
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Json), Object);
		return Object.IsValid() && FFlockMultiplayerSessionReading::FromWireObject(Object.ToSharedRef(), Out, Error);
	};
	FFlockMultiplayerSessionReading Reading;
	FString Error;
	TestTrue(TEXT("an empty connection is none"), Parse(TEXT(R"JSON({"id":"s","connection_info":{},"participants":[]})JSON"), Reading, Error)
		&& !Reading.bHasConnection);
	TestTrue(TEXT("a connection with a mode is one"), Parse(TEXT(R"JSON({"id":"s","connection_info":{"mode":"relay","Port":1}})JSON"), Reading, Error)
		&& Reading.bHasConnection && Reading.Connection.Mode.Equals(TEXT("relay"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("its values keep the mode and their keys as sent"),
		Reading.Connection.Values.ToJsonString().Contains(TEXT("\"Port\""), ESearchCase::CaseSensitive)
		&& Reading.Connection.Values.ToJsonString().Contains(TEXT("\"mode\""), ESearchCase::CaseSensitive));
	TestFalse(TEXT("a seat with no player fails the read"), Parse(TEXT(R"JSON({"id":"s","participants":[{"status":"joined"}]})JSON"), Reading, Error));
	TestEqual(TEXT("no interval reads as none"), Parse(TEXT(R"JSON({"id":"s"})JSON"), Reading, Error) ? Reading.HeartbeatIntervalSeconds : -1, 0);
	return true;
}

// ---- Flock going away, and the game quitting ----

FLOCK_SESSION_TEST(FFlockSessionShutdownTest, "FlockShuttingDownEndsTheSessionWithoutRaisingEnded")
bool FFlockSessionShutdownTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Provider->Shutdown();
	TestTrue(TEXT("ended"), Session->HasEnded());
	TestEqualSensitive(TEXT("as signed out"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	TestEqual(TEXT("not raised"), Listener->Ended.Num(), 0);
	const int32 Sent = F.Server->Requests.Num();
	TestEqual(TEXT("a call afterwards is refused"), F.Leave(Session)->FailureType(), EFlockErrorType::Validation);
	F.Advance(20.0);
	F.Advance(20.0);
	TestEqual(TEXT("nothing sent after, no leave and no heartbeat"), F.Server->Requests.Num(), Sent);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionAnswerAfterShutdownTest, "AnAnswerLandingAfterFlockShutDownHoldsNothing")
bool FFlockSessionAnswerAfterShutdownTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Host);
	const FSessionAnswerRef Hosted = F.Host();
	TestTrue(TEXT("the host is on its way"), Answer->bArrived);
	// The shutdown keeps the sign-in; only the owner's own stop can tell the answer is late.
	F.Provider->Shutdown();
	Answer->Release();
	TestEqual(TEXT("an answer for a Flock that has gone holds nothing"), Hosted->FailureType(), EFlockErrorType::Cancelled);
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionOwnerGoneTest, "ASessionsCallsAfterFlockIsGoneAreRefused")
bool FFlockSessionOwnerGoneTest::RunTest(const FString& Parameters)
{
	// A game keeps the session past a Flock shutdown, which destroys the session's owner with its provider.
	const TSharedRef<FFlockFakeSessionServer> Server = MakeShared<FFlockFakeSessionServer>();
	const TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Server, Log);
	const TSharedRef<FFlockAuthSession> Session = MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), Log,
		TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } });
	TSharedPtr<FFlockMultiplayerProvider> Provider = MakeShared<FFlockMultiplayerProvider>(Client, FSessionsFixture::Retries(0), Log,
		Session, TEXT("http://x/v1"), 0.0);
	FString Error;
	Session->SetTokens(SessionJwt(SessionPlayerA), TEXT("r-1"), Error);
	const FSessionAnswerRef Hosted = NewAnswer();
	Provider->HostSession(TOptional<int32>(), FFlockCommandData(), true, Into(Hosted));
	const TStrongObjectPtr<UFlockMultiplayerSession> Kept(Hosted->Result.Value);
	if (!TestTrue(TEXT("precondition: hosted"), Hosted->Succeeded() && Kept.IsValid()))
	{
		return false;
	}
	Provider->Shutdown();
	Provider.Reset();

	const int32 Sent = Server->Requests.Num();
	const FSessionAnswerRef Left = NewAnswer();
	Kept->Leave(Into(Left));
	const TSharedRef<FTokenAnswer> Token = MakeShared<FTokenAnswer>();
	Kept->RequestJoinToken(Into(Token));
	TestEqual(TEXT("leave refused"), Left->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("token refused"), Token->FailureType(), EFlockErrorType::Validation);
	TestTrue(TEXT("the session reads as ended"), Kept->HasEnded());
	TestEqual(TEXT("no request"), Server->Requests.Num(), Sent);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionQuitTest, "QuittingGivesTheSeatUpWithoutWaitingAndRaisesNothing")
bool FFlockSessionQuitTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Leave);
	TestTrue(TEXT("a leave was sent"), F.Sessions().LeaveForQuit());
	TestTrue(TEXT("on its way, not waited for"), Answer->bArrived && !Answer->bDelivered);
	TestEqualSensitive(TEXT("the session ended as left"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::Left));
	TestEqual(TEXT("raising nothing"), Listener->Ended.Num(), 0);
	TestNull(TEXT("nothing held"), F.Held());
	TestFalse(TEXT("a second quit sends nothing"), F.Sessions().LeaveForQuit());
	Answer->Release();
	TestEqual(TEXT("the answer raises nothing either"), Listener->Ended.Num(), 0);
	TestEqual(TEXT("one leave"), F.Server->Count(ERoute::Leave), 1);
	TestEqualSensitive(TEXT("sent as the player"), F.Server->PlayersOf(ERoute::Leave)[0], SessionPlayerA);
	TestTrue(TEXT("the server passed hosting on"), F.Server->Sessions[0]->HostId == SessionPlayerB);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionQuitSignedOutTest, "QuittingAfterTheSignInEndedSendsNothing")
bool FFlockSessionQuitSignedOutTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.SignOut();
	TestFalse(TEXT("nothing to give up for a sign-in that ended"), F.Sessions().LeaveForQuit());
	TestEqual(TEXT("no leave"), F.Server->Count(ERoute::Leave), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionGameEndingTest, "TheGameInstanceEndingGivesTheSeatUpAndShutdownSdkKeepsIt")
bool FFlockSessionGameEndingTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bAnalyticsWas = Settings->bAnalyticsEnabled;
	// Analytics off: then nothing else waits for the requests on their way, so the leave must have that wait of its own.
	Settings->bAnalyticsEnabled = false;
	ON_SCOPE_EXIT
	{
		Settings->bAnalyticsEnabled = bAnalyticsWas;
	};
	for (const bool bGameEnding : { false, true })
	{
		const FString Case = bGameEnding ? TEXT("the game instance ending") : TEXT("ShutdownSdk");
		const TSharedRef<FFlockFakeSessionServer> Server = MakeShared<FFlockFakeSessionServer>();
		const TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		Store->Stored.AccessToken = SessionJwt(SessionPlayerA);
		Store->Stored.RefreshToken = TEXT("r-1");
		Store->bHasTokens = true;
		FFlockTestSdk Test;
		Test->SetHttpAdapterForTesting(Server);
		Test->SetTokenStoreForTesting(Store);
		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("my-game");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = FFlockFakeSessionServer::GameBuild;
		Test->InitializeWithConfig(Config);
		FFlockMultiplayerProvider* Provider = Test->GetMultiplayerProvider();
		if (!TestNotNull(FString::Printf(TEXT("%s: multiplayer is up"), *Case), Provider) || !TestTrue(TEXT("signed in"), Test->IsAuthenticated()))
		{
			continue;
		}
		const FSessionAnswerRef Hosted = NewAnswer();
		Provider->HostSession(TOptional<int32>(), FFlockCommandData(), true, Into(Hosted));
		if (!TestTrue(FString::Printf(TEXT("%s: hosted"), *Case), Hosted->Succeeded()))
		{
			continue;
		}
		const TStrongObjectPtr<UFlockMultiplayerSession> Kept(Hosted->Result.Value);
		if (bGameEnding)
		{
			Test->Deinitialize();
		}
		else
		{
			Test->ShutdownSdk();
		}
		TestEqual(FString::Printf(TEXT("%s: leaves sent"), *Case), Server->Count(ERoute::Leave), bGameEnding ? 1 : 0);
		TestEqual(FString::Printf(TEXT("%s: the seat on the server"), *Case), Server->Sessions[0]->IsSeated(SessionPlayerA), !bGameEnding);
		TestTrue(FString::Printf(TEXT("%s: the session ended here"), *Case), Kept->HasEnded());
	}
	return true;
}

// ---- heartbeats ----

FLOCK_SESSION_TEST(FFlockSessionHeartbeatIntervalTest, "AHeldSessionHeartbeatsAtTheIntervalItsAnswersGive")
bool FFlockSessionHeartbeatIntervalTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.Server->HeartbeatSeconds = 15;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Advance(14.0);
	TestEqual(TEXT("none before the interval"), F.Server->Count(ERoute::Heartbeat), 0);
	F.Advance(1.0);
	TestEqual(TEXT("one at the interval"), F.Server->Count(ERoute::Heartbeat), 1);
	F.Advance(15.0);
	TestEqual(TEXT("and another one interval on"), F.Server->Count(ERoute::Heartbeat), 2);
	TestEqualSensitive(TEXT("each sent as the player"), FString::Join(F.Server->PlayersOf(ERoute::Heartbeat), TEXT(",")),
		FString(TEXT("player-a,player-a")));
	TestTrue(TEXT("each to the held session"), F.Server->RequestsTo(ERoute::Heartbeat)[0].Url.Contains(Session->GetId(), ESearchCase::CaseSensitive));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatDefaultTest, "AnAnswerGivingNoIntervalHeartbeatsEveryTwentySecondsUntilOneGivesIt")
bool FFlockSessionHeartbeatDefaultTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.Server->HeartbeatSeconds.Reset();
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Advance(19.0);
	TestEqual(TEXT("none before twenty seconds"), F.Server->Count(ERoute::Heartbeat), 0);
	F.Server->HeartbeatSeconds = 6;
	F.Advance(1.0);
	TestEqual(TEXT("one at twenty seconds"), F.Server->Count(ERoute::Heartbeat), 1);
	F.Advance(6.0);
	TestEqual(TEXT("then at the interval its answer gave"), F.Server->Count(ERoute::Heartbeat), 2);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatNewIntervalTest, "TheHeartbeatsFollowANewIntervalTheSessionGives")
bool FFlockSessionHeartbeatNewIntervalTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->HeartbeatSeconds = 8;
	F.Get(Session->GetId());
	TestEqual(TEXT("the heartbeat follows the reading's interval"),
		F.Sessions().GetHeartbeatForTesting().IsValid() ? F.Sessions().GetHeartbeatForTesting()->GetIntervalSeconds() : 0.0, 8.0);
	F.Advance(8.0);
	TestEqual(TEXT("sent at the new interval"), F.Server->Count(ERoute::Heartbeat), 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatRefreshesTest, "AHeartbeatsAnswerRefreshesTheSessionWithoutTheGameReadingIt")
bool FFlockSessionHeartbeatRefreshesTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Advance(20.0);
	TestEqualSensitive(TEXT("B is seen"), PlayersOf(Session), FString(TEXT("player-a,player-b")));
	TestEqual(TEXT("players changed"), Listener->PlayersChanged, 1);
	TestEqual(TEXT("with no read"), F.Server->Count(ERoute::Read) + F.Server->Count(ERoute::Current), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatRefusedTest, "AHeartbeatRefusedForAnotherReasonEndsNothingWarnsOnceAndAReadingStartsThemAgain")
bool FFlockSessionHeartbeatRefusedTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::Heartbeat, FFlockFakeSessionServer::Refused(400, TEXT("request.validation_failed")));
	F.Advance(20.0);
	F.Advance(20.0);
	F.Advance(20.0);
	TestEqual(TEXT("the refused heartbeat was the last"), F.Server->Count(ERoute::Heartbeat), 1);
	TestFalse(TEXT("nothing ended"), Session->HasEnded());
	TestEqual(TEXT("not read to learn why: the refusal does not say the seat is gone"), F.Server->Count(ERoute::Read), 0);
	TestEqual(TEXT("one warning"), F.Log->Warnings.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("no longer kept seated")); }).Num(), 1);
	F.Get(Session->GetId());
	F.Advance(20.0);
	TestEqual(TEXT("a reading started them again"), F.Server->Count(ERoute::Heartbeat), 2);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionNoHeartbeatAfterLeaveTest, "NoHeartbeatAfterLeaving")
bool FFlockSessionNoHeartbeatAfterLeaveTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Advance(20.0);
	TestEqual(TEXT("precondition: heartbeating"), F.Server->Count(ERoute::Heartbeat), 1);
	F.Leave(Session);
	F.Advance(20.0);
	F.Advance(20.0);
	TestEqual(TEXT("none after the leave"), F.Server->Count(ERoute::Heartbeat), 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatFollowsTest, "HeartbeatsFollowTheHeldSessionToTheNextOne")
bool FFlockSessionHeartbeatFollowsTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* First = F.Hosted(*this);
	UFlockMultiplayerSession* Second = F.Hosted(*this);
	if (First == nullptr || Second == nullptr)
	{
		return false;
	}
	F.Advance(20.0);
	const TArray<FFlockHttpRequest> Beats = F.Server->RequestsTo(ERoute::Heartbeat);
	if (TestEqual(TEXT("one heartbeat"), Beats.Num(), 1))
	{
		TestTrue(TEXT("for the second session"), Beats[0].Url.Contains(Second->GetId(), ESearchCase::CaseSensitive));
	}
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionNoHeartbeatAfterSignOutTest, "NoHeartbeatAfterTheSignInEnds")
bool FFlockSessionNoHeartbeatAfterSignOutTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.SignIn(SessionPlayerB);
	F.Advance(20.0);
	F.Advance(20.0);
	TestEqual(TEXT("no heartbeat for A's session as B"), F.Server->Count(ERoute::Heartbeat), 0);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatSeatGoneTest, "AHeartbeatThatFindsTheSeatGoneEndsTheSessionAsDropped")
bool FFlockSessionHeartbeatSeatGoneTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Server->Drops(F.Server->Sessions[0], SessionPlayerA);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Advance(20.0);
	TestEqualSensitive(TEXT("ended as dropped"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Dropped));
	TestEqual(TEXT("read once to learn why"), F.Server->Count(ERoute::Read), 1);
	TestEqual(TEXT("a seat the server gave up is an outcome the game hears of, not a failed call"), F.Log->Errors.Num(), 0);
	F.Advance(20.0);
	TestEqual(TEXT("no heartbeat after"), F.Server->Count(ERoute::Heartbeat), 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionUnreadableWhileLeavingTest, "ASessionNoLongerReadableWhileThisGamesLeaveIsOnItsWayEndsAsLeft")
bool FFlockSessionUnreadableWhileLeavingTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const TSharedRef<FFlockFakePartyServer::FHeld> LeaveAnswer = F.Server->HoldNext(ERoute::Leave);
	F.Leave(Session);
	TestTrue(TEXT("precondition: the leave is on its way"), LeaveAnswer->bArrived && !LeaveAnswer->bDelivered);
	// The next heartbeat finds the seat gone, and the read that would say why is refused.
	F.Server->AnswerNext(ERoute::Read, FFlockFakeSessionServer::Refused(404, TEXT("multiplayer.session_not_found")));
	F.Advance(20.0);
	TestEqual(TEXT("precondition: a heartbeat was refused and the read that followed it too"), F.Server->Count(ERoute::Read), 1);
	TestEqualSensitive(TEXT("gone because this game left it"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Left));
	LeaveAnswer->Release();
	TestEqualSensitive(TEXT("ended once"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::Left));
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionOlderHeartbeatTest, "AnOlderHeartbeatAnswerChangesNothing")
bool FFlockSessionOlderHeartbeatTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	const TSharedRef<FFlockFakePartyServer::FHeld> Beat = F.Server->HoldNext(ERoute::Heartbeat);
	F.Advance(20.0);
	TestTrue(TEXT("a heartbeat is on its way"), Beat->bArrived);
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Get(Session->GetId());
	TestEqualSensitive(TEXT("the newer read showed B"), PlayersOf(Session), FString(TEXT("player-a,player-b")));
	Beat->Release();
	TestEqualSensitive(TEXT("the older heartbeat did not take B away"), PlayersOf(Session), FString(TEXT("player-a,player-b")));
	TestEqual(TEXT("players changed once"), Listener->PlayersChanged, 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatRetryAfterSwitchTest, "ARetriedHeartbeatAfterAPlayerSwitchIsNotSentAsTheNewPlayer")
bool FFlockSessionHeartbeatRetryAfterSwitchTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F(/*MaxRetries*/ 1);
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::Heartbeat, FFlockFakeSessionServer::Refused(503, TEXT("server.unavailable")));
	const TSharedRef<FFlockFakePartyServer::FHeld> FirstTry = F.Server->HoldNext(ERoute::Heartbeat);
	F.Advance(20.0);
	TestTrue(TEXT("the heartbeat's first try is on its way"), FirstTry->bArrived);
	F.SignIn(SessionPlayerB);
	FirstTry->Release();
	// Only the retry's own wait runs: no provider frame, which would end A's session and stop the heartbeat for another reason.
	FSessionsFixture::PumpRetries();
	TestEqual(TEXT("the retry did not go out as B"), F.Server->Count(ERoute::Heartbeat), 1);
	TestEqualSensitive(TEXT("every heartbeat went out as A"), FString::Join(F.Server->PlayersOf(ERoute::Heartbeat), TEXT(",")), SessionPlayerA);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHeartbeatRetryControlTest, "ARetriedHeartbeatIsSentAgainForTheSameSignIn")
bool FFlockSessionHeartbeatRetryControlTest::RunTest(const FString& Parameters)
{
	// The control for the switch test above: with no switch the same 503 is tried again.
	FSessionsFixture F(/*MaxRetries*/ 1);
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::Heartbeat, FFlockFakeSessionServer::Refused(503, TEXT("server.unavailable")));
	F.Advance(20.0);
	FSessionsFixture::PumpRetries();
	TestEqual(TEXT("tried again"), F.Server->Count(ERoute::Heartbeat), 2);
	return true;
}

// ---- my session ----

FLOCK_SESSION_TEST(FFlockSessionMineTest, "GetMySessionIsNullSeatedNowhereReturnsTheHeldObjectAndHoldsTheSessionTheServerSeatsThePlayerIn")
bool FFlockSessionMineTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	const FSessionAnswerRef None = F.GetMine();
	TestTrue(TEXT("seated nowhere is null, not a failure"), None->Succeeded() && None->Result.Value == nullptr);
	TestEqual(TEXT("not logged as a failed call (the HTTP trace notes every 4xx as a warning)"), F.Log->Errors.Num(), 0);

	UFlockMultiplayerSession* Session = F.Hosted(*this);
	const FSessionAnswerRef Held = F.GetMine();
	TestTrue(TEXT("the held object"), Held->Succeeded() && Held->Result.Value == Session);

	// After a restart, or a seat taken on another device: the server's seat is found and held.
	FSessionsFixture Again;
	Again.SignIn(SessionPlayerC);
	Again.Server->HostedBy(SessionPlayerC);
	const FSessionAnswerRef Found = Again.GetMine();
	TestTrue(TEXT("a seat taken elsewhere is found and held"), Found->Succeeded() && Found->Result.Value != nullptr && Again.Held() == Found->Result.Value);
	TestTrue(TEXT("as the host"), Found->Result.Value != nullptr && Found->Result.Value->IsHost());
	Again.Advance(20.0);
	TestEqual(TEXT("and kept seated"), Again.Server->Count(ERoute::Heartbeat), 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionMineSeatGoneTest, "GetMySessionWhenTheSeatIsGoneEndsTheHeldSessionWithTheReasonAReadingGives")
bool FFlockSessionMineSeatGoneTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	F.Server->Ends(F.Server->Sessions[0], TEXT("expired"));
	const FSessionAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("seated nowhere"), Mine->Succeeded() && Mine->Result.Value == nullptr);
	TestEqualSensitive(TEXT("the held session ended with the server's reason"), Listener->EndedAs(), FString(TEXT("expired")));
	TestEqual(TEXT("read once to learn why"), F.Server->Count(ERoute::Read), 1);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionMineElsewhereTest, "GetMySessionSeatedElsewhereEndsTheHeldOneAsMovedToAnotherSession")
bool FFlockSessionMineElsewhereTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	UFlockSessionTestListener* Listener = F.Listen(Session);
	// The player joined another session from another device.
	const FFlockFakeSessionServer::FSessionRef Theirs = F.Server->HostedBy(SessionPlayerB);
	F.Server->Joins(Theirs, SessionPlayerA);
	const FSessionAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("the other session"), Mine->Succeeded() && Mine->Result.Value != nullptr && Mine->Result.Value->GetId() == Theirs->Id);
	TestEqualSensitive(TEXT("the held one ended as moved"), Listener->EndedAs(), FString(FlockMultiplayerSessionEndReasons::MovedToAnotherSession));
	TestTrue(TEXT("the other is held"), F.Held() == Mine->Result.Value);
	return true;
}

// ---- same version only ----

FLOCK_SESSION_TEST(FFlockSessionSameVersionSentTest, "HostSendsSameVersionOnlyOnlyWhenFalse")
bool FFlockSessionSameVersionSentTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	F.Host(TOptional<int32>(), FFlockCommandData(), /*bSameVersionOnly*/ true);
	F.Host(TOptional<int32>(), FFlockCommandData(), /*bSameVersionOnly*/ false);
	const TArray<FFlockHttpRequest> Hosts = F.Server->RequestsTo(ERoute::Host);
	if (!TestEqual(TEXT("two hosts"), Hosts.Num(), 2))
	{
		return false;
	}
	TestFalse(TEXT("true is the server's default, not sent"), SessionBodyOf(Hosts[0])->HasField(TEXT("same_version_only")));
	bool bSent = true;
	TestTrue(TEXT("false is sent"), SessionBodyOf(Hosts[1])->TryGetBoolField(TEXT("same_version_only"), bSent) && !bSent);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionOtherBuildTest, "JoiningASessionOfAnotherBuildIsRefusedOnItsCodeUnlessItsHostOpenedItToAnyBuild")
bool FFlockSessionOtherBuildTest::RunTest(const FString& Parameters)
{
	FSessionsFixture F;
	const FFlockFakeSessionServer::FSessionRef Kept = F.Server->HostedBy(SessionPlayerB);
	Kept->GameVersion = TEXT("ver-old");
	const FFlockFakeSessionServer::FSessionRef Open = F.Server->HostedBy(SessionPlayerC);
	Open->GameVersion = TEXT("ver-old");
	Open->bSameVersionOnly = false;
	F.SignIn(SessionPlayerA);
	TestTrue(TEXT("another build is refused on its code"), F.Join(Kept->Code)->FailedWith(EFlockErrorCode::MultiplayerVersionMismatch));
	TestNull(TEXT("nothing held"), F.Held());
	TestTrue(TEXT("a session open to any build is joined"), F.Join(Open->Code)->Succeeded());
	return true;
}

// ---- the wire ----

FLOCK_SESSION_TEST(FFlockSessionEveryRouteTest, "EveryRouteIsSentAsItsSignIn")
bool FFlockSessionEveryRouteTest::RunTest(const FString& Parameters)
{
	// Every session route goes through SendAs, which binds it to its sign-in (pinned by the sign-in rules' list).
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__))),
		TEXT(".."), TEXT("Multiplayer"), TEXT("FlockMultiplayerSessionRequests.cpp")));
	FString Text;
	if (!TestTrue(TEXT("the requests can be read"), FFileHelper::LoadFileToString(Text, *File)))
	{
		return false;
	}
	const TCHAR* Routes[] = { TEXT("Host"), TEXT("Join"), TEXT("Read"), TEXT("ReadCurrent"), TEXT("Heartbeat"), TEXT("Leave"), TEXT("End"),
		TEXT("MakeHost"), TEXT("PublishConnection"), TEXT("RequestJoinToken"), TEXT("VerifyJoinToken") };
	for (const TCHAR* Route : Routes)
	{
		const FString Start = FString::Printf(TEXT("FFlockRequestHandle FFlockMultiplayerSessionRequests::%s("), Route);
		const int32 At = Text.Find(Start, ESearchCase::CaseSensitive);
		if (!TestTrue(FString::Printf(TEXT("%s is defined"), Route), At != INDEX_NONE))
		{
			continue;
		}
		const int32 End = Text.Find(TEXT("\n}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		const FString Body = Text.Mid(At, End - At);
		TestTrue(FString::Printf(TEXT("%s is sent through SendAs"), Route),
			Body.Contains(TEXT("return SendAs<"), ESearchCase::CaseSensitive) || Body.Contains(TEXT("return PostAs<"), ESearchCase::CaseSensitive));
	}
	// And in behaviour: every route a game drives went out as the player who was signed in.
	FSessionsFixture F;
	F.SignIn(SessionPlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(F.Server->Sessions[0], SessionPlayerB);
	F.Get(Session->GetId());
	F.GetMine();
	F.Advance(20.0);
	F.Publish(Session, TEXT("direct"));
	Session->RequestJoinToken([](TFlockResult<FFlockMultiplayerSessionJoinToken>) {});
	Session->VerifyJoinToken(TEXT("token-x"), [](TFlockResult<FString>) {});
	F.MakeHost(Session, SessionPlayerB);
	F.Leave(Session);
	for (const FFlockHttpRequest& Request : F.Server->Requests)
	{
		TestEqualSensitive(FString::Printf(TEXT("%s went out as A"), *Request.Url), FFlockFakePartyServer::PlayerOf(Request), SessionPlayerA);
	}
	TestEqual(TEXT("nine routes were driven"), F.Server->Requests.Num(), 9);
	return true;
}

FLOCK_SESSION_TEST(FFlockSessionHintsTest, "TheSessionHintsNameCallsTheSdkHas")
bool FFlockSessionHintsTest::RunTest(const FString& Parameters)
{
	struct FNamed
	{
		EFlockErrorCode Code;
		const TCHAR* Call;
	};
	const FNamed Hints[] = {
		{ EFlockErrorCode::MultiplayerSessionNotFound, TEXT("JoinSession") },
		{ EFlockErrorCode::MultiplayerNotHost, TEXT("GetSession") },
		{ EFlockErrorCode::MultiplayerInvalidJoinToken, TEXT("RequestJoinToken") },
		{ EFlockErrorCode::MultiplayerInvalidJoinToken, TEXT("JoinSession") },
		{ EFlockErrorCode::MultiplayerJoinCodeUnavailable, TEXT("HostSession") },
		{ EFlockErrorCode::MultiplayerVersionMismatch, TEXT("HostSession") },
	};
	FString Declared;
	const FString Root = FPaths::Combine(FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__))), TEXT(".."), TEXT(".."), TEXT("Public"));
	for (const TCHAR* Header : { TEXT("Providers/FlockMultiplayerProvider.h"), TEXT("Multiplayer/FlockMultiplayerSession.h") })
	{
		FString Text;
		if (TestTrue(FString::Printf(TEXT("%s can be read"), Header), FFileHelper::LoadFileToString(Text, *FPaths::Combine(Root, Header))))
		{
			Declared += Text;
		}
	}
	auto NamedAsAWord = [](const FString& Text, const FString& Word)
	{
		for (int32 At = Text.Find(Word, ESearchCase::CaseSensitive); At != INDEX_NONE;
			At = Text.Find(Word, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
		{
			const int32 After = At + Word.Len();
			const bool bStarts = At == 0 || !FChar::IsAlnum(Text[At - 1]);
			const bool bEnds = After >= Text.Len() || !FChar::IsAlnum(Text[After]);
			if (bStarts && bEnds)
			{
				return true;
			}
		}
		return false;
	};
	for (const FNamed& Named : Hints)
	{
		const FString Hint = FFlockErrorHints::For(Named.Code);
		TestTrue(FString::Printf(TEXT("the hint for %d names %s"), static_cast<int32>(Named.Code), Named.Call), NamedAsAWord(Hint, Named.Call));
		TestTrue(FString::Printf(TEXT("%s is a call the SDK declares"), Named.Call),
			Declared.Contains(FString::Printf(TEXT("FFlockRequestHandle %s("), Named.Call), ESearchCase::CaseSensitive));
	}
	return true;
}

#undef FLOCK_SESSION_TEST

#endif // WITH_AUTOMATION_TESTS
