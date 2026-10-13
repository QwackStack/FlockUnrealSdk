// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Config/FlockConfig.h"
#include "Containers/Ticker.h"
#include "FlockInitConfig.h"
#include "Dom/JsonObject.h"
#include "Http/FlockHttpClient.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Multiplayer/FlockParties.h"
#include "Multiplayer/FlockParty.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockFakePartyServer.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockPartyTestListener.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "UObject/StrongObjectPtr.h"

namespace FlockPartiesTestHelpers
{
	using ERoute = FFlockFakePartyServer::ERoute;

	const FString PlayerA = TEXT("player-a");
	const FString PlayerB = TEXT("player-b");
	const FString PlayerC = TEXT("player-c");
	const FString PlayerD = TEXT("player-d");

	/** A JWT naming PlayerId as its subject; the fake server reads the requester from it. */
	inline FString PartyJwt(const FString& PlayerId, int32 Variant = 0)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + 3600 + Variant;
		FString Payload = FBase64::Encode(FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp));
		Payload.ReplaceInline(TEXT("+"), TEXT("-"));
		Payload.ReplaceInline(TEXT("/"), TEXT("_"));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("h.%s.s"), *Payload);
	}

	/** What a party call answered; bAnswered stays false while its request is held. */
	struct FPartyAnswer
	{
		bool bAnswered = false;
		TFlockResult<UFlockParty*> Result;

		bool Succeeded() const { return bAnswered && Result.bSuccess; }
		UFlockParty* Party() const { return Result.Value; }
		EFlockErrorType FailureType() const { return Result.Error.Type; }
	};
	using FPartyAnswerRef = TSharedRef<FPartyAnswer>;

	inline FFlockOnParty Into(const FPartyAnswerRef& Answer)
	{
		return [Answer](TFlockResult<UFlockParty*> Result)
		{
			Answer->bAnswered = true;
			Answer->Result = Result;
		};
	}

	inline TArray<FString> Ids(const UFlockParty* Party)
	{
		TArray<FString> Ids;
		if (Party != nullptr)
		{
			for (const FFlockPartyMember& Member : Party->GetMembers())
			{
				Ids.Add(Member.PlayerId);
			}
		}
		return Ids;
	}

	inline FString Joined(const TArray<FString>& Ids) { return FString::Join(Ids, TEXT(",")); }

	inline TSharedPtr<FJsonObject> BodyOf(const FFlockHttpRequest& Request)
	{
		TSharedPtr<FJsonObject> Body;
		FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Request.JsonBody), Body);
		return Body;
	}

	/** The party stack as the provider builds it, over a fake party server, with time moved by hand. */
	struct FPartiesFixture
	{
		TSharedRef<double> Now = MakeShared<double>(1000.0);
		TSharedRef<FFlockFakePartyServer> Server = MakeShared<FFlockFakePartyServer>();
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

		explicit FPartiesFixture(double RefreshSeconds = 10.0, int32 MaxRetries = 0)
			: Client(MakeShared<FFlockHttpClient>(Server, Log))
			, Session(MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), Log, TEXT("http://x/v1"),
				TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") }, { TEXT("X-Game-Version-ID"), TEXT("ver-1") } }))
			, Provider(MakeShared<FFlockMultiplayerProvider>(Client, Retries(MaxRetries), Log, Session, TEXT("http://x/v1"), RefreshSeconds))
		{
			const TSharedRef<double> Clock = Now;
			Provider->GetRepeatingCalls().SetClockForTesting([Clock]() { return *Clock; }, []() { return 0.5; });
		}

		~FPartiesFixture()
		{
			Provider->Shutdown();
		}

		void SignIn(const FString& PlayerId)
		{
			FString Error;
			Session->SetTokens(PartyJwt(PlayerId), TEXT("r-1"), Error);
		}

		void SignOut() { Session->ClearTokens(); }

		/** One frame of the provider: a party of an ended sign-in ends, then the calls due are sent. */
		void Frame() { Provider->TickForTesting(); }

		/** Moves time on and runs a frame; a refresh due by then is sent, and answered unless held. */
		void Advance(double Seconds)
		{
			*Now += Seconds;
			Frame();
		}

		/** Runs the retry handler's scheduled retries, which go through the core ticker. */
		static void PumpRetries()
		{
			for (int32 Index = 0; Index < 8; ++Index)
			{
				FTSTicker::GetCoreTicker().Tick(1.f);
			}
		}

		FFlockParties& Parties() { return Provider->GetPartiesForTesting(); }
		UFlockParty* Held() { return Parties().GetHeld(); }

		FPartyAnswerRef Create(TOptional<int32> Size = TOptional<int32>(), const FFlockCommandData& Settings = FFlockCommandData())
		{
			const FPartyAnswerRef Answer = MakeShared<FPartyAnswer>();
			Provider->CreateParty(Size, Settings, Into(Answer));
			return Answer;
		}

		FPartyAnswerRef Join(const FString& Code)
		{
			const FPartyAnswerRef Answer = MakeShared<FPartyAnswer>();
			Provider->JoinParty(Code, Into(Answer));
			return Answer;
		}

		FPartyAnswerRef GetMine()
		{
			const FPartyAnswerRef Answer = MakeShared<FPartyAnswer>();
			Provider->GetMyParty(Into(Answer));
			return Answer;
		}

		/** A party this test made and kept, or null with the test failed. */
		UFlockParty* MadeParty(FAutomationTestBase& Test, TOptional<int32> Size = TOptional<int32>(),
			const FFlockCommandData& Settings = FFlockCommandData())
		{
			const FPartyAnswerRef Made = Create(Size, Settings);
			Test.TestTrue(TEXT("precondition: the party was made"), Made->Succeeded());
			return Made->Party();
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

		FString EndedAs(const UFlockPartyTestListener* Listener) const
		{
			TArray<FString> Names;
			for (EFlockPartyEndReason Reason : Listener->Ended)
			{
				Names.Add(StaticEnum<EFlockPartyEndReason>()->GetNameStringByValue(static_cast<int64>(Reason)));
			}
			return FString::Join(Names, TEXT(","));
		}
	};
}

using namespace FlockPartiesTestHelpers;

#define FLOCK_PARTY_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Parties." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// ---- getting a party ----

FLOCK_PARTY_TEST(FFlockPartyCreateTest, "CreateHoldsThePlayerAsItsOnlyMemberAndLeaderAndSendsOnlyWhatWasGiven")
bool FFlockPartyCreateTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	const FPartyAnswerRef Made = F.Create();
	if (!TestTrue(TEXT("the party was made"), Made->Succeeded()) || !TestNotNull(TEXT("a party"), Made->Party()))
	{
		return false;
	}
	UFlockParty* Party = Made->Party();
	TestEqualSensitive(TEXT("its only member is the player"), Joined(Ids(Party)), PlayerA);
	TestTrue(TEXT("the player leads it"), Party->IsLeader());
	TestEqualSensitive(TEXT("the server's id"), Party->GetId(), FString(TEXT("party-1")));
	TestEqualSensitive(TEXT("the server's invite code"), Party->GetInviteCode(), FString(TEXT("INV001")));
	TestEqual(TEXT("the server's own default size"), Party->GetMaxSize(), 4);
	TestFalse(TEXT("not ended"), Party->HasEnded());
	TestEqualSensitive(TEXT("nothing given, nothing sent"), F.Server->Requests[0].JsonBody, FString(TEXT("{}")));
	TestEqual(TEXT("the new party needs no read"), F.Server->Count(ERoute::Mine) + F.Server->Count(ERoute::Read), 0);
	TestTrue(TEXT("held"), F.Held() == Party);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyCreateSendsTest, "CreateSendsTheSizeAndSettingsAsGiven")
bool FFlockPartyCreateSendsTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this, 3, FFlockCommandData().Set(TEXT("Mode"), TEXT("duo")).Set(TEXT("level"), 3));
	if (Party == nullptr)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Sent = BodyOf(F.Server->Requests[0]);
	int32 SentSize = 0;
	const TSharedPtr<FJsonObject>* SentSettings = nullptr;
	TestTrue(TEXT("the size was sent"), Sent.IsValid() && Sent->TryGetNumberField(TEXT("max_size"), SentSize) && SentSize == 3);
	TestTrue(TEXT("the settings were sent"), Sent.IsValid() && Sent->TryGetObjectField(TEXT("settings"), SentSettings));
	if (SentSettings != nullptr)
	{
		TestTrue(TEXT("a setting's key is sent as written"), FlockTestSpelling::HasMemberSpelled(*SentSettings, TEXT("Mode")));
	}
	TestEqual(TEXT("the size held"), Party->GetMaxSize(), 3);
	TestTrue(TEXT("a setting's key is kept as written"), FlockTestSpelling::HoldsExactly(Party->GetSettings().GetFieldNames(), TEXT("Mode")));
	FString Mode;
	TestTrue(TEXT("a setting reads back"), Party->GetSettings().TryGetString(TEXT("Mode"), Mode) && Mode == TEXT("duo"));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyJoinTest, "JoinReadsTheMembersBeforeHandingItOver")
bool FFlockPartyJoinTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	FFlockFakePartyServer::FParty& OnServer = F.Server->MadeBy(PlayerB);
	F.Server->Joins(OnServer.Id, PlayerC);
	F.SignIn(PlayerA);
	const FString Code = TEXT("  ") + OnServer.InviteCode.ToLower();
	const FPartyAnswerRef Answer = F.Join(Code);
	if (!TestTrue(TEXT("the party was joined"), Answer->Succeeded()))
	{
		return false;
	}
	UFlockParty* Party = Answer->Party();
	TestEqualSensitive(TEXT("every member, in the order they joined"), Joined(Ids(Party)), Joined({ PlayerB, PlayerC, PlayerA }));
	TestEqualSensitive(TEXT("B leads"), Party->GetLeaderPlayerId(), PlayerB);
	TestFalse(TEXT("the player does not lead"), Party->IsLeader());
	TestEqual(TEXT("one read of the party joined"), F.Server->Count(ERoute::Read), 1);
	FString SentCode;
	const TSharedPtr<FJsonObject> Sent = BodyOf(F.Server->Requests[0]);
	TestTrue(TEXT("the code is sent as the game gave it"), Sent.IsValid() && Sent->TryGetStringField(TEXT("invite_code"), SentCode));
	TestEqualSensitive(TEXT("the code is not changed"), SentCode, Code);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyGetMineTest, "GetMyPartyIsNullInNoneAndTheSameObjectWhileThePartyLasts")
bool FFlockPartyGetMineTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	const FPartyAnswerRef None = F.GetMine();
	TestTrue(TEXT("read"), None->Succeeded());
	TestNull(TEXT("in no party"), None->Party());

	UFlockParty* Party = F.MadeParty(*this);
	const FPartyAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("read again"), Mine->Succeeded());
	TestTrue(TEXT("the same object"), Mine->Party() == Party && Party != nullptr);
	TestTrue(TEXT("and again"), F.GetMine()->Party() == Party);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyMyPartyWireTest, "MyPartyIsReadFromTheEnvelopeAndAMissingResultIsNotAnAnswer")
bool FFlockPartyMyPartyWireTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	const TSharedRef<FJsonObject> NoResult = MakeShared<FJsonObject>();
	NoResult->SetStringField(TEXT("detail"), TEXT("a page that is not the server's"));
	F.Server->AnswerNext(ERoute::Mine, FFlockFakePartyServer::Answer(200, NoResult));
	const FPartyAnswerRef Misread = F.GetMine();
	TestTrue(TEXT("answered"), Misread->bAnswered);
	TestFalse(TEXT("an answer with no result member is not 'in no party'"), Misread->Result.bSuccess);
	TestEqual(TEXT("a misread answer"), Misread->FailureType(), EFlockErrorType::Serialization);
	TestNull(TEXT("nothing held"), F.Held());

	// A member with no player id is not a member.
	F.Server->MadeBy(PlayerA);
	const TSharedRef<FJsonObject> Member = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> Party = MakeShared<FJsonObject>();
	Party->SetStringField(TEXT("id"), TEXT("party-9"));
	Party->SetStringField(TEXT("leader_player_id"), PlayerA);
	Party->SetArrayField(TEXT("members"), { MakeShared<FJsonValueObject>(Member) });
	const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetObjectField(TEXT("result"), Party);
	F.Server->AnswerNext(ERoute::Mine, FFlockFakePartyServer::Answer(200, Envelope));
	const FPartyAnswerRef NoPlayer = F.GetMine();
	TestEqual(TEXT("a member with no player id fails the read"), NoPlayer->FailureType(), EFlockErrorType::Serialization);
	TestNull(TEXT("still nothing held"), F.Held());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyNamesNoPartyTest, "AnAnswerThatNamesNoPartyIsAFailureAndHoldsNothing")
bool FFlockPartyNamesNoPartyTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	const TSharedRef<FJsonObject> Nameless = MakeShared<FJsonObject>();
	Nameless->SetStringField(TEXT("id"), TEXT(""));
	const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetObjectField(TEXT("result"), Nameless);
	F.Server->AnswerNext(ERoute::Create, FFlockFakePartyServer::Answer(200, Envelope));
	const FPartyAnswerRef Made = F.Create();
	TestTrue(TEXT("answered"), Made->bAnswered);
	TestEqual(TEXT("an answer naming no party is a misread one"), Made->FailureType(), EFlockErrorType::Serialization);
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

// ---- the newest reading wins ----

FLOCK_PARTY_TEST(FFlockPartyReadBeforeKickTest, "AReadSentBeforeThisGamesKickFinishedChangesNothing")
bool FFlockPartyReadBeforeKickTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this, 8);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	F.Server->Joins(Party->GetId(), PlayerC);
	F.Advance(1.0);
	if (!TestEqual(TEXT("precondition: a refresh showed B and C"), Ids(Party).Num(), 3))
	{
		return false;
	}

	UFlockPartyTestListener* Listener = F.Listen(Party);
	F.Server->Joins(Party->GetId(), PlayerD);
	const TSharedRef<FFlockFakePartyServer::FHeld> RefreshBeforeTheKick = F.Server->HoldNext(ERoute::Mine);
	F.Advance(1.0);
	TestTrue(TEXT("a refresh that sees D is on its way"), RefreshBeforeTheKick->bArrived);
	const TSharedRef<FFlockFakePartyServer::FHeld> ReadAfterTheKick = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerC, Into(Kicked));
	TestTrue(TEXT("the kick went through and its read is on its way"), ReadAfterTheKick->bArrived);

	RefreshBeforeTheKick->Release();
	TestEqual(TEXT("a read sent before the kick finished changes nothing"), Listener->MembersChanged, 0);
	TestEqualSensitive(TEXT("members as they were"), Joined(Ids(Party)), Joined({ PlayerA, PlayerB, PlayerC }));

	ReadAfterTheKick->Release();
	TestTrue(TEXT("the kick finished"), Kicked->Succeeded());
	TestEqualSensitive(TEXT("the read after the kick is taken"), Joined(Ids(Party)), Joined({ PlayerA, PlayerB, PlayerD }));
	TestEqual(TEXT("one change"), Listener->MembersChanged, 1);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyOlderReadTest, "AnOlderReadLandingAfterANewerOneChangesNothing")
bool FFlockPartyOlderReadTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this, 8);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> OlderRefresh = F.Server->HoldNext(ERoute::Mine);
	F.Advance(1.0);
	TestTrue(TEXT("a refresh that sees A and B is on its way"), OlderRefresh->bArrived);

	F.Server->Joins(Party->GetId(), PlayerC);
	UFlockPartyTestListener* Listener = F.Listen(Party);
	TestTrue(TEXT("a newer read landed"), F.GetMine()->Succeeded());
	TestEqualSensitive(TEXT("the newer read is taken"), Joined(Ids(Party)), Joined({ PlayerA, PlayerB, PlayerC }));

	OlderRefresh->Release();
	TestEqualSensitive(TEXT("the older read did not take C away"), Joined(Ids(Party)), Joined({ PlayerA, PlayerB, PlayerC }));
	TestEqual(TEXT("one change"), Listener->MembersChanged, 1);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyReadBeforeLeavingTest, "AReadSentBeforeLeavingDoesNotBringThePartyBack")
bool FFlockPartyReadBeforeLeavingTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	UFlockPartyTestListener* Listener = F.Listen(Party);

	const TSharedRef<FFlockFakePartyServer::FHeld> ReadBeforeLeaving = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("a read that sees the party is on its way"), ReadBeforeLeaving->bArrived);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Left));
	TestTrue(TEXT("the player left"), Left->Succeeded());

	ReadBeforeLeaving->Release();
	TestTrue(TEXT("the read landed"), Mine->Succeeded());
	TestNull(TEXT("the player is in no party: the old read was read again"), Mine->Party());
	TestNull(TEXT("nothing held"), F.Held());
	TestEqualSensitive(TEXT("ended once, as left"), F.EndedAs(Listener), FString(TEXT("Left")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyGetMineBeforeKickTest, "AGetMyPartySentBeforeAKickReadsAgainAndShowsTheKick")
bool FFlockPartyGetMineBeforeKickTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this, 8);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	F.Server->Joins(Party->GetId(), PlayerC);

	const TSharedRef<FFlockFakePartyServer::FHeld> ReadBeforeTheKick = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Mine = F.GetMine();
	const TSharedRef<FFlockFakePartyServer::FHeld> ReadAfterTheKick = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerC, Into(Kicked));
	TestTrue(TEXT("the kick went through and its read is on its way"), ReadAfterTheKick->bArrived);

	ReadBeforeTheKick->Release();
	TestTrue(TEXT("the old read landed"), Mine->Succeeded());
	TestEqualSensitive(TEXT("the call read again and shows the kick"), Joined(Ids(Mine->Party())), Joined({ PlayerA, PlayerB }));
	ReadAfterTheKick->Release();
	TestTrue(TEXT("the kick finished"), Kicked->Succeeded());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyReadBeforeCreatingTest, "AReadSentBeforeCreatingDoesNotEndTheNewParty")
bool FFlockPartyReadBeforeCreatingTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	const TSharedRef<FFlockFakePartyServer::FHeld> ReadBeforeCreating = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("a read that finds no party is on its way"), ReadBeforeCreating->bArrived);
	UFlockParty* Party = F.MadeParty(*this);

	ReadBeforeCreating->Release();
	TestTrue(TEXT("the read landed"), Mine->Succeeded());
	TestFalse(TEXT("the new party did not end"), Party != nullptr && Party->HasEnded());
	TestTrue(TEXT("the old read was read again and found the new party"), Mine->Party() == Party);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyReadBeforeJoiningTest, "AReadSentBeforeJoiningDoesNotSayThePlayerIsInNoParty")
bool FFlockPartyReadBeforeJoiningTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	FFlockFakePartyServer::FParty& OnServer = F.Server->MadeBy(PlayerB);
	const FString Code = OnServer.InviteCode;
	F.SignIn(PlayerA);
	const TSharedRef<FFlockFakePartyServer::FHeld> ReadBeforeJoining = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Mine = F.GetMine();
	// The join goes through and its own read is held, so the old read lands while the join's read is still on its way.
	const TSharedRef<FFlockFakePartyServer::FHeld> ReadOfTheJoin = F.Server->HoldNext(ERoute::Read);
	const FPartyAnswerRef Joining = F.Join(Code);
	TestTrue(TEXT("the join's read is on its way"), ReadOfTheJoin->bArrived);

	ReadBeforeJoining->Release();
	TestTrue(TEXT("the read landed"), Mine->Succeeded());
	TestNotNull(TEXT("the old read was read again and found the party joined, not 'in no party'"), Mine->Party());
	ReadOfTheJoin->Release();
	TestTrue(TEXT("the party was joined"), Joining->Succeeded());
	TestTrue(TEXT("one party object"), Mine->Party() == Joining->Party());
	TestFalse(TEXT("the party joined did not end"), Joining->Party() != nullptr && Joining->Party()->HasEnded());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyKeepsChangingTest, "AReadOvertakenEveryTimeGivesUpAndSaysWhy")
bool FFlockPartyKeepsChangingTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	// Every read the call sends is held, and overtaken by a change finishing before it lands.
	TSharedRef<FFlockFakePartyServer::FHeld> Pending = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Mine = F.GetMine();
	for (int32 Round = 1; Round <= FFlockParties::MostReadsWhileThePartyChanges; ++Round)
	{
		const FPartyAnswerRef Updated = MakeShared<FPartyAnswer>();
		Party->Update(TOptional<int32>(), FFlockCommandData().Set(TEXT("round"), Round), Into(Updated));
		TestTrue(FString::Printf(TEXT("round %d: the change finished"), Round), Updated->Succeeded());
		const TSharedRef<FFlockFakePartyServer::FHeld> Next = F.Server->HoldNext(ERoute::Mine);
		Pending->Release();
		Pending = Next;
	}
	TestTrue(TEXT("the call gave up"), Mine->bAnswered && !Mine->Result.bSuccess);
	TestTrue(TEXT("it says the party kept changing"), Mine->Result.Error.Message.Contains(TEXT("kept changing")));
	TestEqual(TEXT("it read as many times as it may"), F.Server->Count(ERoute::Mine) - FFlockParties::MostReadsWhileThePartyChanges,
		FFlockParties::MostReadsWhileThePartyChanges);
	return true;
}

// ---- sign-in ----

FLOCK_PARTY_TEST(FFlockPartySignOutTest, "SignOutEndsThePartyAtOnceRaisesEndedOnTheNextFrameAndStopsTheRefresh")
bool FFlockPartySignOutTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	F.Advance(1.0);
	TestEqual(TEXT("precondition: the party was refreshed"), F.Server->Count(ERoute::Mine), 1);

	F.SignOut();
	TestTrue(TEXT("ended in the same frame as the sign-out"), Party->HasEnded());
	TestEqual(TEXT("as signed out"), Party->GetEndReason(), EFlockPartyEndReason::SignedOut);
	TestEqual(TEXT("not raised before the next frame"), Listener->Ended.Num(), 0);
	const FPartyAnswerRef Refused = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Refused));
	TestEqual(TEXT("a call on it is refused"), Refused->FailureType(), EFlockErrorType::Validation);

	F.Frame();
	TestEqualSensitive(TEXT("raised once on the next frame"), F.EndedAs(Listener), FString(TEXT("SignedOut")));
	const int32 Sent = F.Server->Count(ERoute::Mine);
	F.Advance(5.0);
	F.Advance(5.0);
	TestEqualSensitive(TEXT("still once"), F.EndedAs(Listener), FString(TEXT("SignedOut")));
	TestEqual(TEXT("nothing is refreshed after the sign-out"), F.Server->Count(ERoute::Mine), Sent);
	TestEqual(TEXT("the refused kick sent nothing"), F.Server->Count(ERoute::Kick), 0);
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartySwitchTest, "APlayerSwitchEndsTheOldPlayersPartyAndTheNewPlayerGetsTheirOwn")
bool FFlockPartySwitchTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* PartyOfA = F.MadeParty(*this);
	if (PartyOfA == nullptr)
	{
		return false;
	}
	F.Server->Joins(PartyOfA->GetId(), PlayerB);
	UFlockPartyTestListener* Listener = F.Listen(PartyOfA);

	// PlayerB signs in with no sign-out between, and asks in the same frame, before the frame check runs.
	F.SignIn(PlayerB);
	const FPartyAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("B's party was read"), Mine->Succeeded());
	TestTrue(TEXT("B gets a party object of its own"), Mine->Party() != nullptr && Mine->Party() != PartyOfA);
	TestFalse(TEXT("B's party has not ended"), Mine->Party() != nullptr && Mine->Party()->HasEnded());
	TestFalse(TEXT("B does not lead"), Mine->Party() != nullptr && Mine->Party()->IsLeader());
	TestEqualSensitive(TEXT("A's party ended as signed out"), F.EndedAs(Listener), FString(TEXT("SignedOut")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartySwitchThenCreateTest, "APlayerSwitchThenCreatingAPartyEndsTheOldPlayersPartyAsSignedOut")
bool FFlockPartySwitchThenCreateTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* PartyOfA = F.MadeParty(*this);
	if (PartyOfA == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(PartyOfA);
	F.SignIn(PlayerB);
	UFlockParty* PartyOfB = F.MadeParty(*this);
	TestEqualSensitive(TEXT("A's party ended as signed out"), F.EndedAs(Listener), FString(TEXT("SignedOut")));
	TestTrue(TEXT("B's party is held"), PartyOfB != nullptr && F.Held() == PartyOfB);
	TestTrue(TEXT("B leads B's party"), PartyOfB != nullptr && PartyOfB->IsLeader());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyLeaveAfterSwitchTest, "ALeaveAnsweredAfterTheSignInEndedEndsThePartyAsSignedOut")
bool FFlockPartyLeaveAfterSwitchTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Leave);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Left));
	TestTrue(TEXT("A's leave is on its way"), Answer->bArrived);

	// PlayerB signs in, and PlayerA's leave lands before the frame check runs.
	F.SignIn(PlayerB);
	Answer->Release();
	F.Frame();
	TestEqualSensitive(TEXT("ended once, with the reason it already read as"), F.EndedAs(Listener), FString(TEXT("SignedOut")));
	TestEqual(TEXT("signed out"), Party->GetEndReason(), EFlockPartyEndReason::SignedOut);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyLateChangeAnswerTest, "ALateChangeAnswerFromTheLastSignInDoesNotSendTheNewPlayersReadAgain")
bool FFlockPartyLateChangeAnswerTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> Kick = F.Server->HoldNext(ERoute::Kick);
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kicked));
	TestTrue(TEXT("A's kick is on its way"), Kick->bArrived);

	F.SignIn(PlayerD);
	const int32 ReadsBefore = F.Server->Count(ERoute::Mine);
	const TSharedRef<FFlockFakePartyServer::FHeld> Read = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("D's read is on its way"), Read->bArrived);
	Kick->Release();
	TestTrue(TEXT("A's kick landed"), Kicked->bAnswered);
	Read->Release();
	TestTrue(TEXT("D's read was taken"), Mine->Succeeded());
	TestEqual(TEXT("A's answer did not send D's read again"), F.Server->Count(ERoute::Mine) - ReadsBefore, 1);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRetryAfterSwitchTest, "ARetryAfterAPlayerSwitchIsNotSentAsTheNewPlayer")
bool FFlockPartyRetryAfterSwitchTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(10.0, /*MaxRetries*/ 1);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	F.Server->AnswerNext(ERoute::Kick, FFlockFakePartyServer::Refused(429, TEXT("request.rate_limited")));
	const TSharedRef<FFlockFakePartyServer::FHeld> FirstTry = F.Server->HoldNext(ERoute::Kick);
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kicked));
	TestTrue(TEXT("the kick's first try is on its way"), FirstTry->bArrived);
	F.SignIn(PlayerD);
	FirstTry->Release();
	FPartiesFixture::PumpRetries();

	TestEqual(TEXT("a request whose sign-in ended is cancelled"), Kicked->FailureType(), EFlockErrorType::Cancelled);
	TestEqual(TEXT("the retry did not go out as the new player"), F.Server->Count(ERoute::Kick), 1);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRetryControlTest, "AKickRefusedForALimitIsTriedAgainForTheSameSignIn")
bool FFlockPartyRetryControlTest::RunTest(const FString& Parameters)
{
	// The control for the switch test above: with no switch the same 429 is tried again.
	FPartiesFixture F(10.0, /*MaxRetries*/ 1);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	F.Server->AnswerNext(ERoute::Kick, FFlockFakePartyServer::Refused(429, TEXT("request.rate_limited")));
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kicked));
	FPartiesFixture::PumpRetries();
	TestTrue(TEXT("the kick went through on its second try"), Kicked->Succeeded());
	TestEqual(TEXT("two tries, both as A"), Joined(F.Server->PlayersOf(ERoute::Kick)), Joined({ PlayerA, PlayerA }));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyCreateAfterSwitchTest, "ACallWhoseSignInEndedOnTheWayIsCancelledAndHoldsNothing")
bool FFlockPartyCreateAfterSwitchTest::RunTest(const FString& Parameters)
{
	{
		FPartiesFixture F;
		F.SignIn(PlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Create);
		const FPartyAnswerRef Made = F.Create();
		F.SignOut();
		Answer->Release();
		TestEqual(TEXT("create: cancelled"), Made->FailureType(), EFlockErrorType::Cancelled);
		TestNull(TEXT("create: nothing held"), F.Held());
	}
	{
		FPartiesFixture F;
		F.Server->MadeBy(PlayerA);
		F.SignIn(PlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Mine);
		const FPartyAnswerRef Mine = F.GetMine();
		F.SignIn(PlayerB);
		Answer->Release();
		TestEqual(TEXT("get mine: cancelled"), Mine->FailureType(), EFlockErrorType::Cancelled);
		TestNull(TEXT("get mine: nothing held"), F.Held());
	}
	{
		FPartiesFixture F;
		const FString Code = F.Server->MadeBy(PlayerB).InviteCode;
		F.SignIn(PlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Join);
		const FPartyAnswerRef Joining = F.Join(Code);
		F.SignIn(PlayerC);
		Answer->Release();
		TestEqual(TEXT("join: cancelled"), Joining->FailureType(), EFlockErrorType::Cancelled);
		TestNull(TEXT("join: nothing held"), F.Held());
		TestEqual(TEXT("join: the read after it was not sent as C"), F.Server->Count(ERoute::Read), 0);
	}
	return true;
}

FLOCK_PARTY_TEST(FFlockPartySignedOutTest, "SignedOutEveryCallIsRefusedWithoutARequest")
bool FFlockPartySignedOutTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	TestEqual(TEXT("create"), F.Create()->FailureType(), EFlockErrorType::Auth);
	TestEqual(TEXT("join"), F.Join(TEXT("INV001"))->FailureType(), EFlockErrorType::Auth);
	TestEqual(TEXT("get mine"), F.GetMine()->FailureType(), EFlockErrorType::Auth);
	TestEqual(TEXT("no request"), F.Server->Requests.Num(), 0);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyNoPlayerIdTest, "ASignInThatNamesNoPlayerIsRefusedWithoutARequest")
bool FFlockPartyNoPlayerIdTest::RunTest(const FString& Parameters)
{
	// A call's reading is told from a refresh's by the player id, so a sign-in whose token names no player must send nothing.
	FPartiesFixture F;
	F.Server->MadeBy(FString());
	F.SignIn(FString());
	if (!TestTrue(TEXT("precondition: signed in with a token that names no player"), F.Session->IsAuthenticated()
		&& F.Session->GetPlayerId().IsEmpty()))
	{
		return false;
	}
	const FPartyAnswerRef Mine = F.GetMine();
	TestEqual(TEXT("get mine refused"), Mine->FailureType(), EFlockErrorType::Auth);
	TestTrue(TEXT("saying the sign-in names no player"), Mine->Result.Error.Message.Contains(TEXT("names no player")));
	TestEqual(TEXT("create refused"), F.Create()->FailureType(), EFlockErrorType::Auth);
	TestEqual(TEXT("no request"), F.Server->Requests.Num(), 0);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyOwnerGoneTest, "APartysCallsAfterFlockIsGoneAreRefused")
bool FFlockPartyOwnerGoneTest::RunTest(const FString& Parameters)
{
	// A game keeps the party past a Flock shutdown, which destroys the party's owner with its provider.
	const TSharedRef<FFlockFakePartyServer> Server = MakeShared<FFlockFakePartyServer>();
	const TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
	const TSharedRef<FFlockHttpClient> Client = MakeShared<FFlockHttpClient>(Server, Log);
	const TSharedRef<FFlockAuthSession> Session = MakeShared<FFlockAuthSession>(Client, MakeShared<FFlockMemoryTokenStore>(), Log,
		TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } });
	TSharedPtr<FFlockMultiplayerProvider> Provider = MakeShared<FFlockMultiplayerProvider>(Client, FPartiesFixture::Retries(0), Log,
		Session, TEXT("http://x/v1"), 10.0);
	FString Error;
	Session->SetTokens(PartyJwt(PlayerA), TEXT("r-1"), Error);
	const FPartyAnswerRef Made = MakeShared<FPartyAnswer>();
	Provider->CreateParty(TOptional<int32>(), FFlockCommandData(), Into(Made));
	const TStrongObjectPtr<UFlockParty> Kept(Made->Party());
	if (!TestTrue(TEXT("precondition: the party was made"), Made->Succeeded() && Kept.IsValid()))
	{
		return false;
	}
	Provider->Shutdown();
	Provider.Reset();

	const int32 Sent = Server->Requests.Num();
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Kept->Leave(Into(Left));
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Kept->Kick(PlayerB, Into(Kicked));
	TestEqual(TEXT("leave refused"), Left->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("kick refused"), Kicked->FailureType(), EFlockErrorType::Validation);
	TestTrue(TEXT("the party reads as ended"), Kept->HasEnded());
	TestEqual(TEXT("no request"), Server->Requests.Num(), Sent);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyLateReadOfLastSignInTest, "ALateReadFromTheLastSignInLeavesTheNewPlayersPartyAlone")
bool FFlockPartyLateReadOfLastSignInTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* PartyOfA = F.MadeParty(*this);
	if (PartyOfA == nullptr)
	{
		return false;
	}
	F.Server->Joins(PartyOfA->GetId(), PlayerB);
	const TSharedRef<FFlockFakePartyServer::FHeld> ReadAfterTheKick = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	PartyOfA->Kick(PlayerB, Into(Kicked));
	TestTrue(TEXT("A's kick went through and its read is on its way"), ReadAfterTheKick->bArrived);

	F.SignIn(PlayerC);
	F.Server->MadeBy(PlayerC);
	const FPartyAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("C's party was read"), Mine->Succeeded() && Mine->Party() != nullptr);
	ReadAfterTheKick->Release();
	TestTrue(TEXT("A's kick finished"), Kicked->bAnswered);

	TestFalse(TEXT("a read A sent changes nothing for C"), Mine->Party() != nullptr && Mine->Party()->HasEnded());
	TestTrue(TEXT("C's party is held"), F.Held() == Mine->Party());
	return true;
}

// ---- the refresh ----

FLOCK_PARTY_TEST(FFlockPartyRefreshIntervalTest, "TheRefreshRunsAtTheIntervalItIsGiven")
bool FFlockPartyRefreshIntervalTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(4.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	const TSharedPtr<FFlockRepeatingCall>& Refresh = F.Parties().GetRefreshForTesting();
	TestTrue(TEXT("a refresh runs"), Refresh.IsValid() && Refresh->IsRunning());
	TestEqual(TEXT("at the interval given"), Refresh.IsValid() ? Refresh->GetIntervalSeconds() : 0.0, 4.0);
	F.Advance(3.9);
	TestEqual(TEXT("nothing before the interval"), F.Server->Count(ERoute::Mine), 0);
	F.Advance(0.2);
	TestEqual(TEXT("one read at the interval"), F.Server->Count(ERoute::Mine), 1);
	F.Advance(4.1);
	TestEqual(TEXT("one more an interval later"), F.Server->Count(ERoute::Mine), 2);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRefreshSettingTest, "TheRefreshIntervalIsTheGamesSetting")
bool FFlockPartyRefreshSettingTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const float Before = Settings->PartyRefreshSeconds;
	Settings->PartyRefreshSeconds = 3.5f;
	{
		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("my-game");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = TEXT("ver-abc");
		FFlockTestSdk Test;
		Test->InitializeWithConfig(Config);
		FFlockMultiplayerProvider* Provider = Test->GetMultiplayerProvider();
		if (TestNotNull(TEXT("multiplayer is built"), Provider))
		{
			TestEqual(TEXT("the parties refresh at Party Refresh Seconds"), Provider->GetPartiesForTesting().GetRefreshIntervalSeconds(), 3.5);
		}
	}
	Settings->PartyRefreshSeconds = Before;
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRefreshZeroTest, "ARefreshSettingOfZeroSendsNothing")
bool FFlockPartyRefreshZeroTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(0.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	TestTrue(TEXT("precondition: the party is held"), Party != nullptr && F.Held() == Party);
	TestFalse(TEXT("no refresh started"), F.Parties().GetRefreshForTesting().IsValid());
	for (int32 Step = 0; Step < 10; ++Step)
	{
		F.Advance(30.0);
	}
	TestEqual(TEXT("nothing read"), F.Server->Count(ERoute::Mine), 0);
	TestEqual(TEXT("off by choice logs nothing"), F.Log->Errors.Num() + F.Log->Warnings.Num(), 0);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyKickedRefreshTest, "AKickedPlayersNextRefreshEndsThePartyAsRemovedAndTheRefreshStops")
bool FFlockPartyKickedRefreshTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	FFlockFakePartyServer::FParty& OnServer = F.Server->MadeBy(PlayerB);
	const FString PartyId = OnServer.Id;
	const FString Code = OnServer.InviteCode;
	F.SignIn(PlayerA);
	const FPartyAnswerRef Joining = F.Join(Code);
	UFlockParty* Party = Joining->Party();
	if (!TestTrue(TEXT("precondition: joined"), Joining->Succeeded() && Party != nullptr))
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	F.Server->Leaves(PartyId, PlayerA);
	F.Advance(1.0);
	TestEqualSensitive(TEXT("the next refresh ended the party as removed"), F.EndedAs(Listener), FString(TEXT("Removed")));
	TestNull(TEXT("nothing held"), F.Held());
	TestTrue(TEXT("the handler found the party over"), Listener->EndedSawPartyOver.Num() == 1 && Listener->EndedSawPartyOver[0]);

	const int32 Sent = F.Server->Count(ERoute::Mine);
	F.Advance(1.0);
	F.Advance(1.0);
	TestEqual(TEXT("nothing more is refreshed"), F.Server->Count(ERoute::Mine), Sent);
	TestEqual(TEXT("ended once"), Listener->Ended.Num(), 1);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyAnotherPartyTest, "ARefreshThatFindsAnotherPartyEndsTheHeldOneAndHoldsNothing")
bool FFlockPartyAnotherPartyTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	FFlockFakePartyServer::FParty& First = F.Server->MadeBy(PlayerB);
	const FString FirstId = First.Id;
	const FString Code = First.InviteCode;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.Join(Code)->Party();
	if (!TestNotNull(TEXT("precondition: joined"), Party))
	{
		return false;
	}
	// The player left and joined another party on another device.
	F.Server->Leaves(FirstId, PlayerA);
	const FString SecondId = F.Server->MadeBy(PlayerC).Id;
	F.Server->Joins(SecondId, PlayerA);
	F.Advance(1.0);
	TestEqual(TEXT("the held party ended as removed"), Party->GetEndReason(), EFlockPartyEndReason::Removed);
	TestNull(TEXT("only a call the game makes takes up another party"), F.Held());
	const int32 Sent = F.Server->Count(ERoute::Mine);
	F.Advance(1.0);
	TestEqual(TEXT("nothing more is refreshed"), F.Server->Count(ERoute::Mine), Sent);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyIdLetterCaseTest, "APartyWhoseIdDiffersOnlyInLetterCaseIsAnotherParty")
bool FFlockPartyIdLetterCaseTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	// The server now has the player in a party whose id is the held one's in capitals.
	FFlockFakePartyServer::FParty* OnServer = F.Server->PartyById(Party->GetId());
	if (!TestNotNull(TEXT("precondition: on the server"), OnServer))
	{
		return false;
	}
	OnServer->Id = OnServer->Id.ToUpper();
	F.Advance(1.0);
	TestEqual(TEXT("the held party is another one and ended"), Party->GetEndReason(), EFlockPartyEndReason::Removed);
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRefreshStoppedTest, "ARefreshStoppedByAFailureWarnsOnceAndGetMyPartyStartsItAgain")
bool FFlockPartyRefreshStoppedTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::Mine, FFlockFakePartyServer::Refused(422, TEXT("request.validation_failed")));
	F.Advance(1.0);
	auto Stops = [&F]()
	{
		return F.Log->Warnings.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("no longer refreshed")); }).Num();
	};
	TestEqual(TEXT("the stop was reported"), Stops(), 1);
	const int32 Sent = F.Server->Count(ERoute::Mine);
	F.Advance(1.0);
	F.Advance(1.0);
	TestEqual(TEXT("a permanent failure stops the refresh"), F.Server->Count(ERoute::Mine), Sent);
	TestEqual(TEXT("reported once"), Stops(), 1);
	TestFalse(TEXT("the player is still in the party"), Party->HasEnded());

	TestTrue(TEXT("the party was read"), F.GetMine()->Succeeded());
	const int32 AfterAsking = F.Server->Count(ERoute::Mine);
	F.Advance(1.0);
	TestEqual(TEXT("the refresh runs again"), F.Server->Count(ERoute::Mine), AfterAsking + 1);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyLeavingStopsRefreshTest, "LeavingStopsTheRefresh")
bool FFlockPartyLeavingStopsRefreshTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Advance(1.0);
	TestEqual(TEXT("precondition: refreshed"), F.Server->Count(ERoute::Mine), 1);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Left));
	TestTrue(TEXT("left"), Left->Succeeded());
	F.Advance(1.0);
	F.Advance(1.0);
	TestEqual(TEXT("nothing more is refreshed"), F.Server->Count(ERoute::Mine), 1);
	return true;
}

// ---- changes by others ----

FLOCK_PARTY_TEST(FFlockPartyOthersChangesTest, "ChangesByOthersRaiseTheirEventsWithThePartyAlreadyUpdated")
bool FFlockPartyOthersChangesTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	FFlockFakePartyServer::FParty& OnServer = F.Server->MadeBy(PlayerB);
	const FString PartyId = OnServer.Id;
	const FString Code = OnServer.InviteCode;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.Join(Code)->Party();
	if (!TestNotNull(TEXT("precondition: joined"), Party))
	{
		return false;
	}
	const TArray<FFlockPartyMember> Before = Party->GetMembers();
	UFlockPartyTestListener* Listener = F.Listen(Party);

	// PlayerB leaves: PlayerA is the only member left, and leads.
	F.Server->Leaves(PartyId, PlayerB);
	F.Advance(1.0);
	TestEqual(TEXT("members changed once"), Listener->MembersChanged, 1);
	TestEqual(TEXT("leader changed once"), Listener->LeaderChanged, 1);
	TestTrue(TEXT("the members handler saw the new members"), Listener->MembersSeen.Num() == 1 && Listener->MembersSeen[0] == 1);
	TestTrue(TEXT("the leader handler saw the player lead"), Listener->LeadersSeen.Num() == 1 && Listener->LeadersSeen[0] == PlayerA);
	TestTrue(TEXT("the player leads"), Party->IsLeader());
	TestEqual(TEXT("a list handed out earlier keeps its members"), Before.Num(), 2);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyNoChangeNoEventTest, "AReadingThatChangesNothingRaisesNothing")
bool FFlockPartyNoChangeNoEventTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	F.Advance(1.0);
	F.Advance(1.0);
	TestEqual(TEXT("two refreshes"), F.Server->Count(ERoute::Mine), 2);
	TestEqual(TEXT("no members change"), Listener->MembersChanged, 0);
	TestEqual(TEXT("no leader change"), Listener->LeaderChanged, 0);
	TestEqual(TEXT("no end"), Listener->Ended.Num(), 0);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartySettingsTest, "SettingsReadEachValueAsTheTypeAsked")
bool FFlockPartySettingsTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this, TOptional<int32>(), FFlockCommandData()
		.Set(TEXT("mode"), TEXT("duo")).Set(TEXT("n"), 7).Set(TEXT("ratio"), 0.5).Set(TEXT("open"), true)
		.SetRawJson(TEXT("nested"), TEXT("{\"x\":1}")));
	if (Party == nullptr)
	{
		return false;
	}
	const FFlockJsonData Settings = Party->GetSettings();
	int32 N = 0;
	float Ratio = 0.f;
	bool bOpen = false;
	FString Mode;
	int32 X = 0;
	TestTrue(TEXT("a number"), Settings.TryGetInt(TEXT("n"), N) && N == 7);
	TestTrue(TEXT("a fraction"), Settings.TryGetFloat(TEXT("ratio"), Ratio) && FMath::IsNearlyEqual(Ratio, 0.5f));
	TestTrue(TEXT("a switch"), Settings.TryGetBool(TEXT("open"), bOpen) && bOpen);
	TestTrue(TEXT("a word"), Settings.TryGetString(TEXT("mode"), Mode) && Mode == TEXT("duo"));
	TestTrue(TEXT("a nested value"), Settings.TryGetInt(TEXT("nested.x"), X) && X == 1);
	TestFalse(TEXT("a missing key"), Settings.TryGetInt(TEXT("missing"), N));
	return true;
}

// ---- changes by this game ----

FLOCK_PARTY_TEST(FFlockPartyMakeLeaderAndUpdateTest, "MakeLeaderAndUpdateReadThePartyAgainAndOnlyWhatWasGivenIsSent")
bool FFlockPartyMakeLeaderAndUpdateTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this, 4, FFlockCommandData().Set(TEXT("mode"), TEXT("duo")));
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);

	const FPartyAnswerRef Updated = MakeShared<FPartyAnswer>();
	Party->Update(TOptional<int32>(), FFlockCommandData().Set(TEXT("region"), TEXT("eu")), Into(Updated));
	TestTrue(TEXT("updated"), Updated->Succeeded());
	const FFlockHttpRequest* Update = F.Server->Requests.FindByPredicate([](const FFlockHttpRequest& Request)
	{
		return FFlockFakePartyServer::RouteOf(Request) == ERoute::Update;
	});
	const TSharedPtr<FJsonObject> Sent = Update != nullptr ? BodyOf(*Update) : nullptr;
	TestTrue(TEXT("the update was sent"), Sent.IsValid());
	TestFalse(TEXT("a size not given is not sent"), Sent.IsValid() && Sent->HasField(TEXT("max_size")));
	FString Region;
	TestTrue(TEXT("the new settings are read"), Party->GetSettings().TryGetString(TEXT("region"), Region) && Region == TEXT("eu"));
	TestFalse(TEXT("new settings replace the old ones"), Party->GetSettings().HasField(TEXT("mode")));
	TestEqualSensitive(TEXT("the read after the change shows B"), Joined(Ids(Party)), Joined({ PlayerA, PlayerB }));

	UFlockPartyTestListener* Listener = F.Listen(Party);
	const FPartyAnswerRef HandedOver = MakeShared<FPartyAnswer>();
	Party->MakeLeader(PlayerB, Into(HandedOver));
	TestTrue(TEXT("leadership was handed over"), HandedOver->Succeeded());
	TestEqual(TEXT("leader changed once"), Listener->LeaderChanged, 1);
	TestFalse(TEXT("the player no longer leads"), Party->IsLeader());
	TestEqual(TEXT("one read after each change"), F.Server->Count(ERoute::Mine), 2);

	const FPartyAnswerRef Refused = MakeShared<FPartyAnswer>();
	Party->Update(6, TOptional<FFlockCommandData>(), Into(Refused));
	TestEqual(TEXT("an update by a player who no longer leads is refused with its code"), Refused->Result.Error.ErrorCode,
		EFlockErrorCode::PartyNotPartyLeader);
	TestFalse(TEXT("the refusal does not end the party"), Party->HasEnded());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyKickTest, "KickReadsThePartyAgainAndRaisesMembersChanged")
bool FFlockPartyKickTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	TestTrue(TEXT("B was read"), F.GetMine()->Succeeded() && Ids(Party).Num() == 2);
	UFlockPartyTestListener* Listener = F.Listen(Party);
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kicked));
	TestTrue(TEXT("kicked"), Kicked->Succeeded());
	TestTrue(TEXT("the answer is the party"), Kicked->Party() == Party);
	TestEqualSensitive(TEXT("B is gone"), Joined(Ids(Party)), PlayerA);
	TestEqual(TEXT("members changed once"), Listener->MembersChanged, 1);
	FString Target;
	const FFlockHttpRequest* Kick = F.Server->Requests.FindByPredicate([](const FFlockHttpRequest& Request)
	{
		return FFlockFakePartyServer::RouteOf(Request) == ERoute::Kick;
	});
	TestTrue(TEXT("the kick names B"), Kick != nullptr && BodyOf(*Kick)->TryGetStringField(TEXT("player_id"), Target) && Target == PlayerB);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRefusalsTest, "RefusalsComeBackWithTheirOwnCodesAndEndNothing")
bool FFlockPartyRefusalsTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this, 2);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	const FString Code = Party->GetInviteCode();

	auto CodeOf = [](const FPartyAnswerRef& Answer) { return Answer->Result.Error.ErrorCode; };
	const FPartyAnswerRef KickLeader = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerA, Into(KickLeader));
	TestEqual(TEXT("the leader cannot be kicked"), CodeOf(KickLeader), EFlockErrorCode::PartyCannotKickLeader);
	const FPartyAnswerRef KickStranger = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerC, Into(KickStranger));
	TestEqual(TEXT("a player outside the party"), CodeOf(KickStranger), EFlockErrorCode::PartyTargetNotAMember);
	TestEqual(TEXT("already in a party"), CodeOf(F.Create()), EFlockErrorCode::PartyAlreadyInParty);
	TestEqual(TEXT("a wrong invite code"), CodeOf(F.Join(TEXT("NOPE00"))), EFlockErrorCode::PartyInvalidInviteCode);
	TestFalse(TEXT("none of them ended the party"), Party->HasEnded());

	// Another player tries to join the full party; a member who does not lead tries to kick.
	F.SignIn(PlayerC);
	TestEqual(TEXT("a full party"), CodeOf(F.Join(Code)), EFlockErrorCode::PartyFull);
	F.SignIn(PlayerB);
	UFlockParty* PartyOfB = F.GetMine()->Party();
	if (TestNotNull(TEXT("B holds the party"), PartyOfB))
	{
		const FPartyAnswerRef NotLeader = MakeShared<FPartyAnswer>();
		PartyOfB->Kick(PlayerA, Into(NotLeader));
		TestEqual(TEXT("a member who does not lead"), CodeOf(NotLeader), EFlockErrorCode::PartyNotPartyLeader);
		TestEqual(TEXT("a 403 that means not allowed"), NotLeader->Result.Error.StatusCode, 403);
		TestFalse(TEXT("B's party did not end"), PartyOfB->HasEnded());
	}
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyDisbandTest, "DisbandEndsThePartyAsDisbandedAndLaterCallsAreRefusedWithoutARequest")
bool FFlockPartyDisbandTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	const FPartyAnswerRef Disbanded = MakeShared<FPartyAnswer>();
	Party->Disband(Into(Disbanded));
	TestTrue(TEXT("disbanded"), Disbanded->Succeeded());
	TestEqualSensitive(TEXT("ended once as disbanded"), F.EndedAs(Listener), FString(TEXT("Disbanded")));
	TestNull(TEXT("nothing held"), F.Held());

	const int32 Sent = F.Server->Requests.Num();
	const FPartyAnswerRef Kick = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kick));
	const FPartyAnswerRef Leave = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Leave));
	TestEqual(TEXT("kick refused"), Kick->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("leave refused"), Leave->FailureType(), EFlockErrorType::Validation);
	TestTrue(TEXT("the refusal names the end"), Leave->Result.Error.Message.Contains(TEXT("(Disbanded)"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("no request"), F.Server->Requests.Num(), Sent);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyAlreadyOutTest, "LeavingWhenTheServerSaysThePlayerIsAlreadyOutStillEndsAsLeft")
bool FFlockPartyAlreadyOutTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	FFlockFakePartyServer::FParty& OnServer = F.Server->MadeBy(PlayerB);
	const FString PartyId = OnServer.Id;
	const FString Code = OnServer.InviteCode;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.Join(Code)->Party();
	if (!TestNotNull(TEXT("precondition: joined"), Party))
	{
		return false;
	}
	F.Server->Leaves(PartyId, PlayerA);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Left));
	TestTrue(TEXT("already out is what leaving asks for"), Left->Succeeded());
	TestEqual(TEXT("left"), Party->GetEndReason(), EFlockPartyEndReason::Left);
	TestFalse(TEXT("not logged as a failure"), FFlockRecordingLogger::AnyContains(F.Log->Errors, TEXT("Leave party")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyLeaveRefusedTest, "ALeaveTheServerRefusesForAnotherReasonKeepsTheParty")
bool FFlockPartyLeaveRefusedTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	FFlockFakePartyServer::FParty& OnServer = F.Server->MadeBy(PlayerB);
	const FString PartyId = OnServer.Id;
	const FString Code = OnServer.InviteCode;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.Join(Code)->Party();
	if (!TestNotNull(TEXT("precondition: joined"), Party))
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	F.Server->AnswerNext(ERoute::Leave, FFlockFakePartyServer::Refused(400, TEXT("request.validation_failed")));
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Left));
	TestEqual(TEXT("the leave failed"), Left->FailureType(), EFlockErrorType::Validation);
	TestTrue(TEXT("control: a real failure is logged as one"), FFlockRecordingLogger::AnyContains(F.Log->Errors, TEXT("Leave party")));
	TestFalse(TEXT("the party is still the player's"), Party->HasEnded());
	TestTrue(TEXT("still held"), F.Held() == Party);

	// The leader removes the player later: that is a removal, whatever the failed leave was about.
	F.Server->Leaves(PartyId, PlayerA);
	F.Advance(1.0);
	TestEqualSensitive(TEXT("a later removal reads as removed"), F.EndedAs(Listener), FString(TEXT("Removed")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRefreshSeesLeaveTest, "ARefreshThatSeesThisGamesLeaveFirstEndsThePartyAsLeftOnce")
bool FFlockPartyRefreshSeesLeaveTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	UFlockPartyTestListener* Listener = F.Listen(Party);
	const TSharedRef<FFlockFakePartyServer::FHeld> LeaveAnswer = F.Server->HoldNext(ERoute::Leave);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Left));
	TestTrue(TEXT("the server has the leave; its answer is held"), LeaveAnswer->bArrived);
	F.Advance(1.0);
	TestEqualSensitive(TEXT("a refresh found the player out first"), F.EndedAs(Listener), FString(TEXT("Left")));

	LeaveAnswer->Release();
	TestTrue(TEXT("the leave finished"), Left->Succeeded());
	TestEqualSensitive(TEXT("the player left; nobody removed them, and the end came once"), F.EndedAs(Listener), FString(TEXT("Left")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyLeaveOfAnotherTest, "ALeaveOnItsWayForOnePartyLeavesAnotherPartysRemovalARemoval")
bool FFlockPartyLeaveOfAnotherTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* First = F.MadeParty(*this);
	if (First == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> LeaveAnswer = F.Server->HoldNext(ERoute::Leave);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	First->Leave(Into(Left));
	TestTrue(TEXT("precondition: the leave of the first is on its way"), LeaveAnswer->bArrived && !LeaveAnswer->bDelivered);
	const FString TheirsId = F.Server->MadeBy(PlayerB).Id;
	const FString TheirsCode = F.Server->PartyById(TheirsId)->InviteCode;
	const FPartyAnswerRef Joined = F.Join(TheirsCode);
	UFlockParty* Theirs = Joined->Party();
	if (!TestTrue(TEXT("precondition: joined B's party"), Joined->Succeeded() && Theirs != nullptr))
	{
		return false;
	}
	const TStrongObjectPtr<UFlockParty> KeepTheirs(Theirs);
	UFlockPartyTestListener* Listener = F.Listen(Theirs);
	// B removes A while the first party's leave is still on its way; A's next refresh finds it out.
	F.Server->Leaves(TheirsId, PlayerA);
	F.Advance(1.0);
	TestEqualSensitive(TEXT("a removal from B's party is a removal, not the first party's leave"), F.EndedAs(Listener), FString(TEXT("Removed")));
	LeaveAnswer->Release();
	TestEqualSensitive(TEXT("and stays so once the leave answers"), F.EndedAs(Listener), FString(TEXT("Removed")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyEarlierLeaveAnsweredTest, "ALeaveAnsweredForAnEarlierPartyKeepsTheNewerDisbandThisGames")
bool FFlockPartyEarlierLeaveAnsweredTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* First = F.MadeParty(*this);
	if (First == nullptr)
	{
		return false;
	}
	const TStrongObjectPtr<UFlockParty> KeepFirst(First);
	const TSharedRef<FFlockFakePartyServer::FHeld> LeaveAnswer = F.Server->HoldNext(ERoute::Leave);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	First->Leave(Into(Left));
	UFlockParty* Second = F.MadeParty(*this);
	if (Second == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("a party replaced while this game's leave of it is on its way was left, not removed"), First->GetEndReason(), EFlockPartyEndReason::Left);
	const TStrongObjectPtr<UFlockParty> KeepSecond(Second);
	UFlockPartyTestListener* Listener = F.Listen(Second);
	const TSharedRef<FFlockFakePartyServer::FHeld> DisbandAnswer = F.Server->HoldNext(ERoute::Disband);
	const FPartyAnswerRef Disbanded = MakeShared<FPartyAnswer>();
	Second->Disband(Into(Disbanded));
	TestTrue(TEXT("precondition: the leave and the disband are on their way"), LeaveAnswer->bArrived && DisbandAnswer->bArrived && !DisbandAnswer->bDelivered);
	LeaveAnswer->Release();
	// The refresh finds the second party gone before its disband answers.
	F.Advance(1.0);
	TestEqualSensitive(TEXT("the first leave's answer did not forget the disband: it is this game's"), F.EndedAs(Listener), FString(TEXT("Disbanded")));
	DisbandAnswer->Release();
	TestEqualSensitive(TEXT("ended once"), F.EndedAs(Listener), FString(TEXT("Disbanded")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyGoneTest, "AChangeToAPartyThatIsGoneEndsItAsRemovedAndSaysWhy")
bool FFlockPartyGoneTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	F.Server->Disbands(Party->GetId());
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kicked));
	TestEqual(TEXT("refused as not found"), Kicked->Result.Error.ErrorCode, EFlockErrorCode::PartyNotFound);
	TestFalse(TEXT("the refusal says what to do"), Kicked->Result.Error.Hint.IsEmpty());
	TestEqual(TEXT("ended as removed"), Party->GetEndReason(), EFlockPartyEndReason::Removed);
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyDisbandGoneTest, "ADisbandAnsweredPartyNotFoundEndsThePartyAsDisbanded")
bool FFlockPartyDisbandGoneTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	const TStrongObjectPtr<UFlockParty> KeepParty(Party);
	UFlockPartyTestListener* Listener = F.Listen(Party);
	F.Server->AnswerNext(ERoute::Disband, FFlockFakePartyServer::Refused(404, TEXT("party.not_found")));
	const FPartyAnswerRef Disbanded = MakeShared<FPartyAnswer>();
	Party->Disband(Into(Disbanded));
	TestEqual(TEXT("the call says the party was gone"), Disbanded->Result.Error.ErrorCode, EFlockErrorCode::PartyNotFound);
	TestEqualSensitive(TEXT("gone already is what disbanding asks for"), F.EndedAs(Listener), FString(TEXT("Disbanded")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyGoneWhileLeavingTest, "AChangeAnsweredPartyNotFoundWhileThisGamesLeaveIsOnItsWayEndsThePartyAsLeft")
bool FFlockPartyGoneWhileLeavingTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	const TStrongObjectPtr<UFlockParty> KeepParty(Party);
	UFlockPartyTestListener* Listener = F.Listen(Party);
	const TSharedRef<FFlockFakePartyServer::FHeld> LeaveAnswer = F.Server->HoldNext(ERoute::Leave);
	const FPartyAnswerRef Left = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Left));
	// The party is gone by the time a change the game sent meanwhile reaches the server.
	F.Server->AnswerNext(ERoute::Kick, FFlockFakePartyServer::Refused(404, TEXT("party.not_found")));
	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kicked));
	TestEqual(TEXT("precondition: refused as gone"), Kicked->Result.Error.ErrorCode, EFlockErrorCode::PartyNotFound);
	TestEqualSensitive(TEXT("gone because this game left it"), F.EndedAs(Listener), FString(TEXT("Left")));
	LeaveAnswer->Release();
	TestEqualSensitive(TEXT("ended once"), F.EndedAs(Listener), FString(TEXT("Left")));
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyEmptyInputsTest, "EmptyInputsAreRefusedWithoutARequest")
bool FFlockPartyEmptyInputsTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	const int32 Sent = F.Server->Requests.Num();
	TestEqual(TEXT("join with no code"), F.Join(FString())->FailureType(), EFlockErrorType::Validation);
	const FPartyAnswerRef Kick = MakeShared<FPartyAnswer>();
	Party->Kick(FString(), Into(Kick));
	TestEqual(TEXT("kick no one"), Kick->FailureType(), EFlockErrorType::Validation);
	const FPartyAnswerRef Lead = MakeShared<FPartyAnswer>();
	Party->MakeLeader(FString(), Into(Lead));
	TestEqual(TEXT("make no one leader"), Lead->FailureType(), EFlockErrorType::Validation);
	const FPartyAnswerRef Nothing = MakeShared<FPartyAnswer>();
	Party->Update(TOptional<int32>(), TOptional<FFlockCommandData>(), Into(Nothing));
	TestEqual(TEXT("update nothing"), Nothing->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("no request"), F.Server->Requests.Num(), Sent);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyEndedHandlerTest, "AnEndHandlerThatCallsBackInFindsThePartyOver")
bool FFlockPartyEndedHandlerTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	UFlockParty* HeldInHandler = Party;
	FPartyAnswerRef LeaveInHandler = MakeShared<FPartyAnswer>();
	Listener->AlsoInHandler = [&]()
	{
		HeldInHandler = F.Held();
		Party->Leave(Into(LeaveInHandler));
	};
	const FPartyAnswerRef Disbanded = MakeShared<FPartyAnswer>();
	Party->Disband(Into(Disbanded));
	TestEqualSensitive(TEXT("ended once"), F.EndedAs(Listener), FString(TEXT("Disbanded")));
	TestTrue(TEXT("the handler found it over"), Listener->EndedSawPartyOver.Num() == 1 && Listener->EndedSawPartyOver[0]);
	TestNull(TEXT("the handler found nothing held"), HeldInHandler);
	TestEqual(TEXT("a call from the handler is refused"), LeaveInHandler->FailureType(), EFlockErrorType::Validation);
	TestEqual(TEXT("and sends nothing"), F.Server->Count(ERoute::Leave), 0);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyCancelTest, "CancellingACallStopsTheRequestOnItsWayAndSendsNoOther")
bool FFlockPartyCancelTest::RunTest(const FString& Parameters)
{
	{
		FPartiesFixture F;
		const FString Code = F.Server->MadeBy(PlayerB).InviteCode;
		F.SignIn(PlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> JoinAnswer = F.Server->HoldNext(ERoute::Join);
		const FPartyAnswerRef Joining = MakeShared<FPartyAnswer>();
		FFlockRequestHandle Handle = F.Provider->JoinParty(Code, Into(Joining));
		Handle.Cancel();
		TestEqual(TEXT("a join cancelled on its way"), Joining->FailureType(), EFlockErrorType::Cancelled);
		TestEqual(TEXT("no read after it"), F.Server->Count(ERoute::Read), 0);
		TestNull(TEXT("nothing held"), F.Held());
	}
	{
		FPartiesFixture F;
		const FString Code = F.Server->MadeBy(PlayerB).InviteCode;
		F.SignIn(PlayerA);
		const TSharedRef<FFlockFakePartyServer::FHeld> ReadAnswer = F.Server->HoldNext(ERoute::Read);
		const FPartyAnswerRef Joining = MakeShared<FPartyAnswer>();
		FFlockRequestHandle Handle = F.Provider->JoinParty(Code, Into(Joining));
		TestTrue(TEXT("the join went through and its read is on its way"), ReadAnswer->bArrived);
		Handle.Cancel();
		TestEqual(TEXT("cancelling reaches the read on its way"), Joining->FailureType(), EFlockErrorType::Cancelled);
		TestNull(TEXT("nothing held"), F.Held());
	}
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyRetriesTest, "ChangesALostAnswerCouldRepeatAreNeverSentTwice")
bool FFlockPartyRetriesTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(10.0, /*MaxRetries*/ 1);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	// Each call's first answer is a 500, which says nothing about whether the server did it.
	auto Tries = [&F](ERoute Route, TFunction<void()> Call)
	{
		const int32 Before = F.Server->Count(Route);
		F.Server->AnswerNext(Route, FFlockFakePartyServer::Refused(500, TEXT("server.error")));
		Call();
		FPartiesFixture::PumpRetries();
		return F.Server->Count(Route) - Before;
	};
	const FPartyAnswerRef Ignored = MakeShared<FPartyAnswer>();
	TestEqual(TEXT("create: once"), Tries(ERoute::Create, [&]() { F.Create(); }), 1);
	TestEqual(TEXT("kick: once"), Tries(ERoute::Kick, [&]() { Party->Kick(PlayerB, Into(Ignored)); }), 1);
	TestEqual(TEXT("make leader: once"), Tries(ERoute::MakeLeader, [&]() { Party->MakeLeader(PlayerB, Into(Ignored)); }), 1);
	TestEqual(TEXT("disband: once"), Tries(ERoute::Disband, [&]() { Party->Disband(Into(Ignored)); }), 1);
	TestFalse(TEXT("none of those ended the party"), Party->HasEnded());
	// Controls: the calls a repeat cannot hurt are tried again.
	const FString Code = Party->GetInviteCode();
	TestEqual(TEXT("join: again"), Tries(ERoute::Join, [&]() { F.Join(Code); }), 2);
	TestEqual(TEXT("update: again"), Tries(ERoute::Update, [&]() { Party->Update(3, TOptional<FFlockCommandData>(), Into(Ignored)); }), 2);
	TestEqual(TEXT("leave: again"), Tries(ERoute::Leave, [&]() { Party->Leave(Into(Ignored)); }), 2);
	TestEqual(TEXT("the leave went through on its second try"), Party->GetEndReason(), EFlockPartyEndReason::Left);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyTokenAfterRefreshTest, "ACallRefusedAsSignedOutIsSentAgainWithTheRefreshedToken")
bool FFlockPartyTokenAfterRefreshTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	F.Server->Joins(Party->GetId(), PlayerB);
	const FString Refreshed = PartyJwt(PlayerA, 7);
	const TSharedRef<FJsonObject> TokenReply = MakeShared<FJsonObject>();
	TokenReply->SetStringField(TEXT("player_id"), PlayerA);
	TokenReply->SetStringField(TEXT("access_token"), Refreshed);
	TokenReply->SetStringField(TEXT("refresh_token"), TEXT("r-2"));
	F.Server->AnswerNext(ERoute::Kick, FFlockFakePartyServer::Refused(401, TEXT("player.invalid_access_token")));
	F.Server->AnswerNext(ERoute::Other, FFlockFakePartyServer::Answer(200, TokenReply));

	const FPartyAnswerRef Kicked = MakeShared<FPartyAnswer>();
	Party->Kick(PlayerB, Into(Kicked));
	TestTrue(TEXT("the kick went through after the refresh"), Kicked->Succeeded());
	TArray<FString> Tokens;
	for (const FFlockHttpRequest& Request : F.Server->Requests)
	{
		if (FFlockFakePartyServer::RouteOf(Request) == ERoute::Kick)
		{
			Tokens.Add(Request.Headers.FindRef(TEXT("Authorization")));
		}
	}
	if (TestEqual(TEXT("sent twice"), Tokens.Num(), 2))
	{
		TestEqualSensitive(TEXT("the replay carries the refreshed token"), Tokens[1], TEXT("Bearer ") + Refreshed);
		TestNotEqual(TEXT("not the first one"), Tokens[0], Tokens[1]);
	}
	TestFalse(TEXT("the sign-in carried on"), Party->HasEnded());
	return true;
}

// ---- Flock going away ----

FLOCK_PARTY_TEST(FFlockPartyShutdownTest, "FlockShuttingDownEndsThePartyWithoutRaisingEnded")
bool FFlockPartyShutdownTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F(1.0);
	F.SignIn(PlayerA);
	UFlockParty* Party = F.MadeParty(*this);
	if (Party == nullptr)
	{
		return false;
	}
	UFlockPartyTestListener* Listener = F.Listen(Party);
	F.Provider->Shutdown();
	TestTrue(TEXT("ended"), Party->HasEnded());
	TestEqual(TEXT("as signed out"), Party->GetEndReason(), EFlockPartyEndReason::SignedOut);
	TestEqual(TEXT("not raised"), Listener->Ended.Num(), 0);
	const int32 Sent = F.Server->Requests.Num();
	const FPartyAnswerRef Leave = MakeShared<FPartyAnswer>();
	Party->Leave(Into(Leave));
	TestEqual(TEXT("a call afterwards is refused"), Leave->FailureType(), EFlockErrorType::Validation);
	F.Advance(5.0);
	TestEqual(TEXT("nothing sent after"), F.Server->Requests.Num(), Sent);
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyAnswerAfterShutdownTest, "AnAnswerLandingAfterFlockShutDownHoldsNothing")
bool FFlockPartyAnswerAfterShutdownTest::RunTest(const FString& Parameters)
{
	FPartiesFixture F;
	F.Server->MadeBy(PlayerA);
	F.SignIn(PlayerA);
	const TSharedRef<FFlockFakePartyServer::FHeld> Answer = F.Server->HoldNext(ERoute::Mine);
	const FPartyAnswerRef Mine = F.GetMine();
	TestTrue(TEXT("the read is on its way"), Answer->bArrived);
	// The shutdown keeps the sign-in; only the owner's own stop can tell the answer is late.
	F.Provider->Shutdown();
	Answer->Release();
	TestEqual(TEXT("an answer for a Flock that has gone holds nothing"), Mine->FailureType(), EFlockErrorType::Cancelled);
	TestNull(TEXT("nothing held"), F.Held());
	return true;
}

FLOCK_PARTY_TEST(FFlockPartyTokenEachTryTest, "EveryRouteIsSentAsItsSignIn")
bool FFlockPartyTokenEachTryTest::RunTest(const FString& Parameters)
{
	// Every party route but "my party" goes through SendAs, which binds it to its sign-in (pinned by the sign-in rules' list).
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::GetPath(FString(ANSI_TO_TCHAR(__FILE__))),
		TEXT(".."), TEXT("Multiplayer"), TEXT("FlockPartyRequests.cpp")));
	FString Text;
	if (!TestTrue(TEXT("the requests can be read"), FFileHelper::LoadFileToString(Text, *File)))
	{
		return false;
	}
	const TCHAR* Routes[] = { TEXT("Create"), TEXT("Join"), TEXT("Read"), TEXT("Leave"), TEXT("Kick"), TEXT("MakeLeader"),
		TEXT("Update"), TEXT("Disband") };
	for (const TCHAR* Route : Routes)
	{
		const FString Start = FString::Printf(TEXT("FFlockRequestHandle FFlockPartyRequests::%s("), Route);
		const int32 At = Text.Find(Start, ESearchCase::CaseSensitive);
		if (!TestTrue(FString::Printf(TEXT("%s is defined"), Route), At != INDEX_NONE))
		{
			continue;
		}
		const int32 End = Text.Find(TEXT("\n}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
		const FString Body = Text.Mid(At, End - At);
		TestTrue(FString::Printf(TEXT("%s is sent through SendAs"), Route), Body.Contains(TEXT("return SendAs("), ESearchCase::CaseSensitive));
	}
	return true;
}

#undef FLOCK_PARTY_TEST

#endif // WITH_AUTOMATION_TESTS
