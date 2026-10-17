// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"
#include "Misc/ScopeExit.h"

#if WITH_AUTOMATION_TESTS

#include "Blueprint/FlockMultiplayerAsyncActions.h"
#include "Blueprint/FlockMultiplayerLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FlockSubsystem.h"
#include "Http/FlockErrorHints.h"
#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Tests/Support/FlockConnectingFixture.h"
#include "Tests/Support/FlockMatchmakingFixture.h"
#include "Tests/Support/FlockMultiplayerNodeTestListener.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace FlockMultiplayerNodesTestHelpers
{
	namespace Matchmaking = FlockMatchmakingTestHelpers;
	namespace Connecting = FlockConnectingTestHelpers;

	/** The listener a test's nodes fire into, and every node kept from garbage collection while the test runs. */
	struct FNodes
	{
		TStrongObjectPtr<UFlockMultiplayerNodeTestListener> Listener{ NewObject<UFlockMultiplayerNodeTestListener>() };
		TArray<TStrongObjectPtr<UObject>> Kept;

		UFlockMultiplayerNodeTestListener* operator->() const { return Listener.Get(); }

		template <typename TAction>
		TAction* Keep(TAction* Action)
		{
			Kept.Emplace(Action);
			return Action;
		}

		UFlockPartyAction* Party(UFlockPartyAction* Action)
		{
			Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::PartySucceeded);
			Action->OnFailure.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::PartyFailed);
			return Keep(Action);
		}

		UFlockSessionAction* Session(UFlockSessionAction* Action)
		{
			Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionSucceeded);
			Action->OnFailure.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionFailed);
			return Keep(Action);
		}

		UFlockFindMatchAction* Search(UFlockFindMatchAction* Action)
		{
			Action->OnMatched.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::Matched);
			Action->OnNoMatch.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::NoMatch);
			Action->OnFailure.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::MatchFailed);
			return Keep(Action);
		}

		UFlockWaitForConnectionAction* Wait(UFlockWaitForConnectionAction* Action)
		{
			Action->OnConnection.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::ConnectionPublished);
			Action->OnSessionEnded.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionEnded);
			Action->OnTimedOut.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::TimedOut);
			Action->OnFailure.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionFailed);
			return Keep(Action);
		}

		UFlockTravelToSessionAction* Travel(UFlockTravelToSessionAction* Action)
		{
			Action->OnJoined.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::Joined);
			Action->OnNotJoined.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::NotJoined);
			Action->OnFailure.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::TravelFailed);
			return Keep(Action);
		}
	};

	inline FFlockCommandData Settings(const FString& Json) { return FFlockCommandData::FromJsonString(Json); }

	inline FString SettingOf(const TSharedRef<FJsonObject>& Settings, const TCHAR* Key)
	{
		FString Value;
		Settings->TryGetStringField(Key, Value);
		return Value;
	}

	/** A world of its own, for an object that belongs to a level, as an actor or a widget does; destroyed when this ends. */
	struct FLevelWorld
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("FlockNodesLevelWorld"));
		~FLevelWorld() { World->DestroyWorld(/*bInformEngineOfWorld*/ false); }
	};

	/** The connecting fixture's session as B, its host A's published direct connection, and the session's server side. */
	struct FTravelFixture : Connecting::FConnectingFixture
	{
		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();

		UFlockMultiplayerSession* PublishedAndJoinedAsB(FAutomationTestBase& Test)
		{
			UFlockMultiplayerSession* Held = JoinedAsB(Test, Served);
			if (Held != nullptr)
			{
				Server->Publishes(Served, Connecting::Direct(Connecting::Text(TEXT("198.51.100.4")), Connecting::Number(7777), TEXT("192.168.1.20"),
					TEXT("198.51.100.4")));
			}
			return Held;
		}
	};
}

using namespace FlockMultiplayerNodesTestHelpers;

#define FLOCK_NODES_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Nodes." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// ---- parties ----

FLOCK_NODES_TEST(FFlockNodesCreatePartyTest, "Party.CreateParty_MaxSizeZeroTakesTheServersDefault_AndSendsTheSettings")
bool FFlockNodesCreatePartyTest::RunTest(const FString& Parameters)
{
	{
		Matchmaking::FMatchmakingFixture F;
		F.SignIn(Matchmaking::PlayerA);
		FNodes Nodes;
		Nodes.Party(UFlockPartyAction::CreateParty(nullptr, 0, Settings(TEXT("{\"mode\":\"duel\"}"))))->ActivateWithProviderForTesting(&*F.Provider);
		TestEqualSensitive(TEXT("the success pin, once"), Nodes->FiredText(), FString(TEXT("success")));
		TestTrue(TEXT("with the party the player now holds"), Nodes->Party != nullptr && Nodes->Party == F.HeldParty());
		FFlockFakePartyServer::FParty* Made = F.Server->Parties->PartyOf(Matchmaking::PlayerA);
		if (TestNotNull(TEXT("made on the server"), Made))
		{
			TestEqual(TEXT("Max Size 0 sends none: the server's default"), Made->MaxSize, 4);
			TestEqualSensitive(TEXT("the settings sent"), SettingOf(Made->Settings, TEXT("mode")), FString(TEXT("duel")));
		}
	}
	{
		Matchmaking::FMatchmakingFixture F;
		F.SignIn(Matchmaking::PlayerA);
		FNodes Nodes;
		Nodes.Party(UFlockPartyAction::CreateParty(nullptr, 6, FFlockCommandData()))->ActivateWithProviderForTesting(&*F.Provider);
		FFlockFakePartyServer::FParty* Made = F.Server->Parties->PartyOf(Matchmaking::PlayerA);
		TestTrue(TEXT("a size given is sent"), Nodes->FiredText() == TEXT("success") && Made != nullptr && Made->MaxSize == 6);
	}
	return true;
}

FLOCK_NODES_TEST(FFlockNodesGetMyPartyTest, "Party.GetMyParty_TellsThePlayersPartyFromNone")
bool FFlockNodesGetMyPartyTest::RunTest(const FString& Parameters)
{
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	auto Ask = [&F](FNodes& Nodes)
	{
		UFlockGetMyPartyAction* Action = Nodes.Keep(UFlockGetMyPartyAction::GetMyParty(nullptr));
		Action->OnParty.AddDynamic(Nodes.Listener.Get(), &UFlockMultiplayerNodeTestListener::PartyFound);
		Action->OnNoParty.AddDynamic(Nodes.Listener.Get(), &UFlockMultiplayerNodeTestListener::NoParty);
		Action->OnFailure.AddDynamic(Nodes.Listener.Get(), &UFlockMultiplayerNodeTestListener::PartyFailed);
		Action->ActivateWithProviderForTesting(&*F.Provider);
	};
	FNodes None;
	Ask(None);
	TestEqualSensitive(TEXT("in no party: No Party"), None->FiredText(), FString(TEXT("no-party")));

	F.PartyOf(Matchmaking::PlayerA, { Matchmaking::PlayerB });
	FNodes Some;
	Ask(Some);
	TestEqualSensitive(TEXT("in a party: Party"), Some->FiredText(), FString(TEXT("party")));
	TestTrue(TEXT("the party the player holds"), Some->Party != nullptr && Some->Party == F.HeldParty());

	F.SignOut();
	FNodes Failed;
	Ask(Failed);
	TestEqualSensitive(TEXT("signed out: Failure"), Failed->FiredText(), FString(TEXT("failure")));
	return true;
}

FLOCK_NODES_TEST(FFlockNodesPartyCallsTest, "Party.TheCallsOnAParty_ActOnIt_AndUpdateKeepsWhatItDoesNotReplace")
bool FFlockNodesPartyCallsTest::RunTest(const FString& Parameters)
{
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	FNodes Made;
	Made.Party(UFlockPartyAction::CreateParty(nullptr, 0, Settings(TEXT("{\"mode\":\"duel\"}"))))->ActivateWithProviderForTesting(&*F.Provider);
	UFlockParty* Party = Made->Party;
	if (!TestNotNull(TEXT("precondition: a party"), Party))
	{
		return false;
	}
	FFlockFakePartyServer::FParty* OnServer = F.Server->Parties->PartyOf(Matchmaking::PlayerA);

	FNodes Sized;
	Sized.Party(UFlockPartyAction::UpdateParty(nullptr, Party, 6, false, Settings(TEXT("{\"mode\":\"ignored\"}"))))->Activate();
	TestEqualSensitive(TEXT("update: success"), Sized->FiredText(), FString(TEXT("success")));
	TestEqual(TEXT("the new size"), OnServer->MaxSize, 6);
	TestEqualSensitive(TEXT("Replace Settings off keeps the settings"), SettingOf(OnServer->Settings, TEXT("mode")), FString(TEXT("duel")));

	FNodes Replaced;
	Replaced.Party(UFlockPartyAction::UpdateParty(nullptr, Party, 0, true, Settings(TEXT("{\"mode\":\"trio\"}"))))->Activate();
	TestEqualSensitive(TEXT("Replace Settings on replaces them"), SettingOf(OnServer->Settings, TEXT("mode")), FString(TEXT("trio")));
	TestEqual(TEXT("New Max Size 0 keeps the size"), OnServer->MaxSize, 6);

	F.Server->Parties->Joins(OnServer->Id, Matchmaking::PlayerB);
	F.Server->Parties->Joins(OnServer->Id, Matchmaking::PlayerC);
	F.GetMine();
	FNodes Kicked;
	Kicked.Party(UFlockPartyAction::KickFromParty(nullptr, Party, Matchmaking::PlayerC))->Activate();
	TestEqualSensitive(TEXT("kick: success"), Kicked->FiredText(), FString(TEXT("success")));
	TestFalse(TEXT("C is out"), OnServer->Members.Contains(Matchmaking::PlayerC));

	FNodes Led;
	Led.Party(UFlockPartyAction::MakePartyLeader(nullptr, Party, Matchmaking::PlayerB))->Activate();
	TestTrue(TEXT("make leader: B leads"), Led->FiredText() == TEXT("success") && Party->GetLeaderPlayerId().Equals(Matchmaking::PlayerB, ESearchCase::CaseSensitive));

	FNodes Left;
	Left.Party(UFlockPartyAction::LeaveParty(nullptr, Party))->Activate();
	TestTrue(TEXT("leave: success, the party over for this player"), Left->FiredText() == TEXT("success") && Left->Party == Party && Party->HasEnded());

	FNodes Again;
	Again.Party(UFlockPartyAction::CreateParty(nullptr, 0, FFlockCommandData()))->ActivateWithProviderForTesting(&*F.Provider);
	FNodes Disbanded;
	Disbanded.Party(UFlockPartyAction::DisbandParty(nullptr, Again->Party))->Activate();
	TestTrue(TEXT("disband: success, the party over"), Disbanded->FiredText() == TEXT("success") && Again->Party != nullptr && Again->Party->HasEnded());
	return true;
}

FLOCK_NODES_TEST(FFlockNodesNoPartyGivenTest, "Party.ANodeGivenNoParty_FailsWithTheFix_AndSendsNothing")
bool FFlockNodesNoPartyGivenTest::RunTest(const FString& Parameters)
{
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	FNodes Nodes;
	Nodes.Party(UFlockPartyAction::LeaveParty(nullptr, nullptr))->Activate();
	TestEqualSensitive(TEXT("the failure pin"), Nodes->FiredText(), FString(TEXT("failure")));
	TestEqual(TEXT("a mistake"), Nodes->Error.Type, EFlockErrorType::Validation);
	TestEqualSensitive(TEXT("with the fix"), Nodes->Error.Hint, FFlockErrorHints::PassAHeldParty());
	FNodes Search;
	Search.Search(UFlockFindMatchAction::PartyFindMatch(nullptr, nullptr, Matchmaking::FServer::Duel, FFlockMatchmakingOptions()))->Activate();
	TestTrue(TEXT("a party's search given no party: the same"), Search->FiredText() == TEXT("failure") && Search->Error.Hint == FFlockErrorHints::PassAHeldParty());
	TestEqual(TEXT("nothing sent"), F.Server->Parties->Count(FFlockFakePartyServer::ERoute::Leave) + F.Server->Count(Matchmaking::ERoute::Create), 0);

	FNodes Join;
	Join.Party(UFlockPartyAction::JoinParty(nullptr, FString()))->ActivateWithProviderForTesting(&*F.Provider);
	TestTrue(TEXT("a call Flock itself refuses fires the failure pin with Flock's error"),
		Join->FiredText() == TEXT("failure") && Join->Error.Type == EFlockErrorType::Validation && !Join->Error.Hint.IsEmpty());

	FNodes NotRunning;
	NotRunning.Party(UFlockPartyAction::CreateParty(nullptr, 0, FFlockCommandData()))->ActivateWithProviderForTesting(nullptr);
	TestTrue(TEXT("Flock not running: the failure pin, naming the fix"),
		NotRunning->FiredText() == TEXT("failure") && NotRunning->Error.Hint == FFlockErrorHints::InitializeFlockFirst());
	return true;
}

// ---- searching for a match ----

FLOCK_NODES_TEST(FFlockNodesMatchedTest, "Match.AMatch_FiresOnMatched_WithTheSessionHeld")
bool FFlockNodesMatchedTest::RunTest(const FString& Parameters)
{
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	F.Server->QueuedBy(Matchmaking::FServer::DuelId, Matchmaking::PlayerB);
	FNodes Nodes;
	Nodes.Search(UFlockFindMatchAction::FindMatch(nullptr, Matchmaking::FServer::Duel, FFlockMatchmakingOptions()))->ActivateWithProviderForTesting(&*F.Provider);
	TestTrue(TEXT("searching: nothing fired"), Nodes->Fired.IsEmpty());
	F.Server->FormMatches();
	F.Advance(3.8);
	TestEqualSensitive(TEXT("Matched, once"), Nodes->FiredText(), FString(TEXT("matched")));
	TestTrue(TEXT("with the session the player now holds"), Nodes->Match.Session != nullptr && Nodes->Match.Session == F.HeldSession());
	TestTrue(TEXT("Is Matched agrees"), UFlockMultiplayerLibrary::IsMatched(Nodes->Match));
	return true;
}

FLOCK_NODES_TEST(FFlockNodesNoMatchTest, "Match.EveryOutcomeWithoutAMatch_FiresOnNoMatch")
bool FFlockNodesNoMatchTest::RunTest(const FString& Parameters)
{
	{
		Matchmaking::FMatchmakingFixture F;
		F.SignIn(Matchmaking::PlayerA);
		FNodes Nodes;
		Nodes.Search(UFlockFindMatchAction::FindMatch(nullptr, Matchmaking::FServer::Duel, FFlockMatchmakingOptions()))->ActivateWithProviderForTesting(&*F.Provider);
		F.Server->ExpiresAll();
		F.Advance(3.8);
		TestEqualSensitive(TEXT("expired: No Match"), Nodes->FiredText(), FString(TEXT("no-match")));
		TestEqualSensitive(TEXT("its outcome"), Nodes->Match.Outcome, UFlockMultiplayerLibrary::MatchmakingOutcomeExpired());
		TestFalse(TEXT("Is Matched agrees"), UFlockMultiplayerLibrary::IsMatched(Nodes->Match));
	}
	{
		Matchmaking::FMatchmakingFixture F;
		F.SignIn(Matchmaking::PlayerA);
		const FString PartyId = F.PartyOf(Matchmaking::PlayerA, { Matchmaking::PlayerB });
		UFlockParty* Held = F.MyParty(*this);
		FNodes Nodes;
		Nodes.Search(UFlockFindMatchAction::PartyFindMatch(nullptr, Held, Matchmaking::FServer::Trio, FFlockMatchmakingOptions()))->Activate();
		F.Server->Parties->Joins(PartyId, Matchmaking::PlayerC);
		F.Advance(3.0);
		TestEqualSensitive(TEXT("party changed: No Match too"), Nodes->FiredText(), FString(TEXT("no-match")));
		TestEqualSensitive(TEXT("its outcome"), Nodes->Match.Outcome, UFlockMultiplayerLibrary::MatchmakingOutcomePartyChanged());
	}
	return true;
}

FLOCK_NODES_TEST(FFlockNodesCancelSearchTest, "Match.Cancel_StopsTheSearchOnTheServer_AndFiresNoPin")
bool FFlockNodesCancelSearchTest::RunTest(const FString& Parameters)
{
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	FNodes Nodes;
	UFlockFindMatchAction* Action = Nodes.Search(UFlockFindMatchAction::FindMatch(nullptr, Matchmaking::FServer::Duel, FFlockMatchmakingOptions()));
	Action->ActivateWithProviderForTesting(&*F.Provider);
	TestTrue(TEXT("searching"), Action->IsActive());
	Action->Cancel();
	F.Advance(3.8);
	TestTrue(TEXT("a cancel the game asked for fires no pin"), Nodes->Fired.IsEmpty());
	TestFalse(TEXT("and the node is done"), Action->IsActive());
	TestEqual(TEXT("the search is cancelled on the server"), F.Server->Count(Matchmaking::ERoute::Cancel), 1);
	TestEqualSensitive(TEXT("the ticket"), F.StatusOf(Matchmaking::PlayerA), FString(TEXT("cancelled")));
	return true;
}

FLOCK_NODES_TEST(FFlockNodesShutdownSearchTest, "Match.FlockShuttingDownWhileSearching_FiresOnFailure")
bool FFlockNodesShutdownSearchTest::RunTest(const FString& Parameters)
{
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	FNodes Nodes;
	Nodes.Search(UFlockFindMatchAction::FindMatch(nullptr, Matchmaking::FServer::Duel, FFlockMatchmakingOptions()))->ActivateWithProviderForTesting(&*F.Provider);
	F.Provider->Shutdown();
	TestEqualSensitive(TEXT("a cancel Flock made itself fires the failure pin"), Nodes->FiredText(), FString(TEXT("failure")));
	return true;
}

FLOCK_NODES_TEST(FFlockNodesKeptTest, "Match.ANodeStillSearching_IsKeptByTheGameInstance_UntilItsPinFires")
bool FFlockNodesKeptTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	auto Search = [](Matchmaking::FMatchmakingFixture& F, UObject* WorldContextObject, UFlockMultiplayerNodeTestListener* Listener)
	{
		F.SignIn(Matchmaking::PlayerA);
		F.Server->QueuedBy(Matchmaking::FServer::DuelId, Matchmaking::PlayerB);
		UFlockFindMatchAction* Action = UFlockFindMatchAction::FindMatch(WorldContextObject, Matchmaking::FServer::Duel, FFlockMatchmakingOptions());
		Action->OnMatched.AddDynamic(Listener, &UFlockMultiplayerNodeTestListener::Matched);
		Action->ActivateWithProviderForTesting(&*F.Provider);
		return TWeakObjectPtr<UFlockFindMatchAction>(Action);
	};
	{
		// The control: a node nothing keeps is gone at the next collection, which is what the game instance prevents.
		Matchmaking::FMatchmakingFixture F;
		const TStrongObjectPtr<UFlockMultiplayerNodeTestListener> Listener(NewObject<UFlockMultiplayerNodeTestListener>());
		const TWeakObjectPtr<UFlockFindMatchAction> Unkept = Search(F, nullptr, Listener.Get());
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("control: a node with no game instance is collected"), Unkept.IsValid());
		F.Server->FormMatches();
		F.Advance(3.8);
		TestTrue(TEXT("control: and its pin never fires"), Listener->Fired.IsEmpty());
	}
	{
		Matchmaking::FMatchmakingFixture F;
		const TStrongObjectPtr<UFlockMultiplayerNodeTestListener> Listener(NewObject<UFlockMultiplayerNodeTestListener>());
		const TWeakObjectPtr<UFlockFindMatchAction> Kept = Search(F, Instance.Get(), Listener.Get());
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestTrue(TEXT("kept while it searches"), Kept.IsValid());
		F.Server->FormMatches();
		F.Advance(3.8);
		TestEqualSensitive(TEXT("and its pin fires"), Listener->FiredText(), FString(TEXT("matched")));
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TestFalse(TEXT("let go once its pin has fired"), Kept.IsValid());
	}
	return true;
}

// ---- sessions ----

FLOCK_NODES_TEST(FFlockNodesHostSessionTest, "Session.HostSession_MaxPlayersZeroTakesTheGamesCount_AndSendsSameVersionOnly")
bool FFlockNodesHostSessionTest::RunTest(const FString& Parameters)
{
	Connecting::FConnectingFixture F;
	F.SignIn(Connecting::PlayerA);
	FNodes Nodes;
	Nodes.Session(UFlockSessionAction::HostSession(nullptr, 0, Settings(TEXT("{\"map\":\"docks\"}")), false))->ActivateWithProviderForTesting(&*F.Provider);
	TestEqualSensitive(TEXT("success"), Nodes->FiredText(), FString(TEXT("success")));
	FFlockFakeSessionServer::FSessionRef Served = Nodes->Session != nullptr ? F.Server->ById(Nodes->Session->GetId()).ToSharedRef()
		: MakeShared<FFlockFakeSessionServer::FSession>();
	TestEqual(TEXT("Max Players 0 sends none: the game's count"), Served->MaxPlayers, 8);
	TestFalse(TEXT("Same Version Only sent as given"), Served->bSameVersionOnly);
	TestTrue(TEXT("held"), Nodes->Session != nullptr && Nodes->Session == F.Sessions().GetHeld());

	FNodes Mine;
	UFlockGetMySessionAction* Action = Mine.Keep(UFlockGetMySessionAction::GetMySession(nullptr));
	Action->OnSession.AddDynamic(Mine.Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionFound);
	Action->OnNoSession.AddDynamic(Mine.Listener.Get(), &UFlockMultiplayerNodeTestListener::NoSession);
	Action->OnFailure.AddDynamic(Mine.Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionFailed);
	Action->ActivateWithProviderForTesting(&*F.Provider);
	TestTrue(TEXT("Get My Session: the session"), Mine->FiredText() == TEXT("session") && Mine->Session == Nodes->Session);

	FNodes Ended;
	Ended.Session(UFlockSessionAction::EndSession(nullptr, Nodes->Session))->Activate();
	TestTrue(TEXT("End Session: success, ended"), Ended->FiredText() == TEXT("success") && Nodes->Session != nullptr && Nodes->Session->HasEnded());

	FNodes None;
	UFlockGetMySessionAction* Again = None.Keep(UFlockGetMySessionAction::GetMySession(nullptr));
	Again->OnSession.AddDynamic(None.Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionFound);
	Again->OnNoSession.AddDynamic(None.Listener.Get(), &UFlockMultiplayerNodeTestListener::NoSession);
	Again->OnFailure.AddDynamic(None.Listener.Get(), &UFlockMultiplayerNodeTestListener::SessionFailed);
	Again->ActivateWithProviderForTesting(&*F.Provider);
	TestEqualSensitive(TEXT("seated nowhere: No Session"), None->FiredText(), FString(TEXT("no-session")));
	return true;
}

FLOCK_NODES_TEST(FFlockNodesSessionCallsTest, "Session.EachSessionCall_ReachesItsOwnCallWithItsOwnInputs")
bool FFlockNodesSessionCallsTest::RunTest(const FString& Parameters)
{
	{
		// The host's calls.
		FTravelFixture F;
		F.SignIn(Connecting::PlayerA);
		UFlockMultiplayerSession* Hosted = F.Hosted(*this);
		if (Hosted == nullptr)
		{
			return false;
		}
		const FFlockFakeSessionServer::FSessionRef Served = F.Server->ById(Hosted->GetId()).ToSharedRef();
		F.Server->Joins(Served, Connecting::PlayerB);

		FNodes Published;
		Published.Session(UFlockSessionAction::PublishConnection(nullptr, Hosted, TEXT("steam"), Settings(TEXT("{\"lobby\":\"109775241\"}"))))->Activate();
		TestTrue(TEXT("Publish Connection: the mode and the values given"), Published->FiredText() == TEXT("success")
			&& Connecting::FConnectingFixture::Field(F.LastPublished(), TEXT("mode")) == TEXT("steam")
			&& Connecting::FConnectingFixture::Field(F.LastPublished(), TEXT("lobby")) == TEXT("109775241"));

		FNodes Direct;
		Direct.Session(UFlockSessionAction::PublishDirectConnection(nullptr, Hosted, 7778))->Activate();
		F.FramesUntil([&]() { return !Direct->Fired.IsEmpty(); });
		double Port = 0.0;
		TestTrue(TEXT("Publish Direct Connection: the port given"), Direct->FiredText() == TEXT("success") && F.LastPublished().IsValid()
			&& F.LastPublished()->TryGetNumberField(TEXT("port"), Port) && Port == 7778.0);

		FNodes Handed;
		Handed.Session(UFlockSessionAction::MakeSessionHost(nullptr, Hosted, Connecting::PlayerB))->Activate();
		TestTrue(TEXT("Make Session Host: the player given hosts"), Handed->FiredText() == TEXT("success")
			&& Served->HostId.Equals(Connecting::PlayerB, ESearchCase::CaseSensitive));

		FNodes Left;
		Left.Session(UFlockSessionAction::LeaveSession(nullptr, Hosted))->Activate();
		TestTrue(TEXT("Leave Session: this player's seat given up"), Left->FiredText() == TEXT("success") && !Served->IsSeated(Connecting::PlayerA)
			&& Hosted->HasEnded());
	}
	{
		// A player's calls: the session by its code, by its id, and where its host is reached.
		FTravelFixture F;
		const FFlockFakeSessionServer::FSessionRef Served = F.Server->HostedBy(Connecting::PlayerA);
		F.Server->Publishes(Served, Connecting::Direct(Connecting::Text(TEXT("198.51.100.4")), Connecting::Number(7777), TEXT("192.168.1.20"),
			TEXT("198.51.100.4")));
		F.SignIn(Connecting::PlayerB);

		FNodes Joined;
		Joined.Session(UFlockSessionAction::JoinSession(nullptr, Served->Code))->ActivateWithProviderForTesting(&*F.Provider);
		TestTrue(TEXT("Join Session: the session with that code"), Joined->FiredText() == TEXT("success") && Joined->Session != nullptr
			&& Joined->Session->GetId().Equals(Served->Id, ESearchCase::CaseSensitive) && Served->IsSeated(Connecting::PlayerB));

		FNodes Read;
		Read.Session(UFlockSessionAction::GetSession(nullptr, Served->Id))->ActivateWithProviderForTesting(&*F.Provider);
		TestTrue(TEXT("Get Session: the session with that id"), Read->FiredText() == TEXT("success") && Read->Session != nullptr
			&& Read->Session->GetId().Equals(Served->Id, ESearchCase::CaseSensitive));

		FNodes Address;
		UFlockFindDirectAddressAction* Find = Address.Keep(UFlockFindDirectAddressAction::FindDirectAddress(nullptr, Joined->Session));
		Find->OnSuccess.AddDynamic(Address.Listener.Get(), &UFlockMultiplayerNodeTestListener::AddressFound);
		Find->OnFailure.AddDynamic(Address.Listener.Get(), &UFlockMultiplayerNodeTestListener::AddressFailed);
		Find->Activate();
		F.FramesUntil([&]() { return !Address->Fired.IsEmpty(); });
		TestTrue(TEXT("Find Direct Address: the host's address and port"), Address->FiredText() == TEXT("success")
			&& !Address->Address.Address.IsEmpty() && Address->Address.Port == 7777);
	}
	return true;
}

FLOCK_NODES_TEST(FFlockNodesCallerKeptTest, "Match.AWaitingNode_DoesNotKeepItsCallerOrItsLevelAlive")
bool FFlockNodesCallerKeptTest::RunTest(const FString& Parameters)
{
	// A node waits minutes, kept by the game instance; what called it may belong to a level the game is leaving. Were the node to
	// hold its caller, the old level's world would outlive the level change, which the engine reports as a world not cleaned up.
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	FLevelWorld Level;
	Level.World->SetGameInstance(Instance.Get());
	ON_SCOPE_EXIT
	{
		Level.World->SetGameInstance(nullptr);
	};
	const TWeakObjectPtr<UObject> Caller = NewObject<UFlockMultiplayerNodeTestListener>(Level.World);
	const TStrongObjectPtr<UFlockMultiplayerNodeTestListener> Listener(NewObject<UFlockMultiplayerNodeTestListener>());
	UFlockFindMatchAction* Action = UFlockFindMatchAction::FindMatch(Caller.Get(), Matchmaking::FServer::Duel, FFlockMatchmakingOptions());
	Action->OnMatched.AddDynamic(Listener.Get(), &UFlockMultiplayerNodeTestListener::Matched);
	Action->ActivateWithProviderForTesting(&*F.Provider);
	const TWeakObjectPtr<UFlockFindMatchAction> Node = Action;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("the node still waits, kept by its caller's game instance"), Node.IsValid() && Node->IsActive());
	TestFalse(TEXT("its caller is not kept alive by it"), Caller.IsValid());
	if (Node.IsValid())
	{
		Node->Cancel();
	}
	return true;
}

FLOCK_NODES_TEST(FFlockNodesNoSessionGivenTest, "Session.ANodeGivenNoSession_FailsWithTheFix")
bool FFlockNodesNoSessionGivenTest::RunTest(const FString& Parameters)
{
	FNodes Leave;
	Leave.Session(UFlockSessionAction::LeaveSession(nullptr, nullptr))->Activate();
	TestTrue(TEXT("Leave Session"), Leave->FiredText() == TEXT("failure") && Leave->Error.Hint == FFlockErrorHints::PassAHeldSession());
	FNodes Wait;
	Wait.Wait(UFlockWaitForConnectionAction::WaitForConnection(nullptr, nullptr, 5.f))->Activate();
	TestTrue(TEXT("Wait For Connection"), Wait->FiredText() == TEXT("failure") && Wait->Error.Hint == FFlockErrorHints::PassAHeldSession());
	FNodes Travel;
	Travel.Travel(UFlockTravelToSessionAction::TravelToSession(nullptr, nullptr, FFlockTravelToSessionOptions()))->Activate();
	TestTrue(TEXT("Travel To Session"), Travel->FiredText() == TEXT("failure") && Travel->Error.Hint == FFlockErrorHints::PassAHeldSession());
	FNodes Token;
	UFlockRequestJoinTokenAction* TokenAction = Token.Keep(UFlockRequestJoinTokenAction::RequestJoinToken(nullptr, nullptr));
	TokenAction->OnFailure.AddDynamic(Token.Listener.Get(), &UFlockMultiplayerNodeTestListener::TokenFailed);
	TokenAction->Activate();
	TestTrue(TEXT("Request Join Token"), Token->FiredText() == TEXT("failure") && Token->Error.Hint == FFlockErrorHints::PassAHeldSession());
	return true;
}

FLOCK_NODES_TEST(FFlockNodesTokensTest, "Session.JoinTokens_RequestedByAPlayer_AndVerifiedByTheHost")
bool FFlockNodesTokensTest::RunTest(const FString& Parameters)
{
	Connecting::FConnectingFixture F;
	F.SignIn(Connecting::PlayerA);
	UFlockMultiplayerSession* Hosted = F.Hosted(*this);
	if (Hosted == nullptr)
	{
		return false;
	}
	FFlockFakeSessionServer::FSessionRef Served = F.Server->ById(Hosted->GetId()).ToSharedRef();
	F.Server->Joins(Served, Connecting::PlayerB);
	const FString Token = F.Server->MintsJoinToken(Served, Connecting::PlayerB);

	FNodes Good;
	UFlockVerifyJoinTokenAction* Verify = Good.Keep(UFlockVerifyJoinTokenAction::VerifyJoinToken(nullptr, Hosted, Token));
	Verify->OnSuccess.AddDynamic(Good.Listener.Get(), &UFlockMultiplayerNodeTestListener::Verified);
	Verify->OnFailure.AddDynamic(Good.Listener.Get(), &UFlockMultiplayerNodeTestListener::VerifyFailed);
	Verify->Activate();
	TestTrue(TEXT("a good token: the player it was given to"), Good->FiredText() == TEXT("success")
		&& Good->PlayerId.Equals(Connecting::PlayerB, ESearchCase::CaseSensitive));

	FNodes Bad;
	UFlockVerifyJoinTokenAction* VerifyBad = Bad.Keep(UFlockVerifyJoinTokenAction::VerifyJoinToken(nullptr, Hosted, TEXT("made-up")));
	VerifyBad->OnSuccess.AddDynamic(Bad.Listener.Get(), &UFlockMultiplayerNodeTestListener::Verified);
	VerifyBad->OnFailure.AddDynamic(Bad.Listener.Get(), &UFlockMultiplayerNodeTestListener::VerifyFailed);
	VerifyBad->Activate();
	TestTrue(TEXT("a made-up token: the failure pin, coded"), Bad->FiredText() == TEXT("failure")
		&& Bad->Error.ErrorCode == EFlockErrorCode::MultiplayerInvalidJoinToken);

	FNodes Mine;
	UFlockRequestJoinTokenAction* Request = Mine.Keep(UFlockRequestJoinTokenAction::RequestJoinToken(nullptr, Hosted));
	Request->OnSuccess.AddDynamic(Mine.Listener.Get(), &UFlockMultiplayerNodeTestListener::TokenGiven);
	Request->OnFailure.AddDynamic(Mine.Listener.Get(), &UFlockMultiplayerNodeTestListener::TokenFailed);
	Request->Activate();
	TestTrue(TEXT("Request Join Token: a token"), Mine->FiredText() == TEXT("success") && !Mine->JoinToken.Token.IsEmpty());
	return true;
}

FLOCK_NODES_TEST(FFlockNodesWaitTest, "Session.WaitForConnection_TellsAConnection_AnEndedSession_AndTimingOut")
bool FFlockNodesWaitTest::RunTest(const FString& Parameters)
{
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.PublishedAndJoinedAsB(*this);
		FNodes Nodes;
		Nodes.Wait(UFlockWaitForConnectionAction::WaitForConnection(nullptr, Session, 5.f))->Activate();
		F.Advance(0.25);
		TestEqualSensitive(TEXT("published: Connection"), Nodes->FiredText(), FString(TEXT("connection")));
	}
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.JoinedAsB(*this, F.Served);
		FNodes Nodes;
		Nodes.Wait(UFlockWaitForConnectionAction::WaitForConnection(nullptr, Session, 5.f))->Activate();
		// Published and ended before the next reading: the reading tells the ending (a player no longer seated is not told the address).
		F.Server->Publishes(F.Served, Connecting::Direct(Connecting::Text(TEXT("198.51.100.4")), Connecting::Number(7777), TEXT("192.168.1.20"),
			TEXT("198.51.100.4")));
		F.Server->Ends(F.Served);
		F.Advance(0.25);
		F.Advance(0.25);
		TestEqualSensitive(TEXT("ended while waiting: Session Ended, not Connection"), Nodes->FiredText(), FString(TEXT("session-ended")));
	}
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.JoinedAsB(*this, F.Served);
		FNodes Nodes;
		Nodes.Wait(UFlockWaitForConnectionAction::WaitForConnection(nullptr, Session, 1.f))->Activate();
		for (int32 Step = 0; Step < 8; ++Step)
		{
			F.Advance(0.25);
		}
		TestEqualSensitive(TEXT("nothing published in time: Timed Out"), Nodes->FiredText(), FString(TEXT("timed-out")));
	}
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.JoinedAsB(*this, F.Served);
		FNodes Nodes;
		UFlockWaitForConnectionAction* Action = Nodes.Wait(UFlockWaitForConnectionAction::WaitForConnection(nullptr, Session, 5.f));
		Action->Activate();
		Action->Cancel();
		F.Advance(6.0);
		TestTrue(TEXT("cancelled: no pin"), Nodes->Fired.IsEmpty());
	}
	return true;
}

// ---- travelling into the session's game ----

FLOCK_NODES_TEST(FFlockNodesTravelJoinedTest, "Travel.ConnectedOrRefused_FiresOnJoinedOrOnNotJoined")
bool FFlockNodesTravelJoinedTest::RunTest(const FString& Parameters)
{
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.PublishedAndJoinedAsB(*this);
		FNodes Nodes;
		Nodes.Travel(UFlockTravelToSessionAction::TravelToSession(nullptr, Session, FFlockTravelToSessionOptions()))->ActivateWithProviderForTesting(&*F.Provider);
		F.Advance(0.25);
		F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
		F.Game->Arrives(true);
		F.Frame();
		TestEqualSensitive(TEXT("connected: Joined"), Nodes->FiredText(), FString(TEXT("joined")));
		TestEqualSensitive(TEXT("its outcome"), Nodes->Travel.Outcome, UFlockMultiplayerLibrary::TravelOutcomeConnected());
	}
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.PublishedAndJoinedAsB(*this);
		FNodes Nodes;
		Nodes.Travel(UFlockTravelToSessionAction::TravelToSession(nullptr, Session, FFlockTravelToSessionOptions()))->ActivateWithProviderForTesting(&*F.Provider);
		F.Advance(0.25);
		F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
		F.Game->Fails(FString(TEXT("PendingConnectionFailure: ")) + FlockHostRefusals::WordsFor(TEXT("banned")));
		F.Frame();
		F.Game->GoesBackToItsDefaultLevel();
		F.Frame();
		TestEqualSensitive(TEXT("refused: Not Joined"), Nodes->FiredText(), FString(TEXT("not-joined")));
		TestEqualSensitive(TEXT("refused"), Nodes->Travel.Outcome, UFlockMultiplayerLibrary::TravelOutcomeRefused());
		TestEqualSensitive(TEXT("with the host's reason"), Nodes->Travel.RefusedReason, FString(TEXT("banned")));
	}
	return true;
}

FLOCK_NODES_TEST(FFlockNodesTravelHostTest, "Travel.AHost_FiresOnJoined_AndCancelFiresNoPin")
bool FFlockNodesTravelHostTest::RunTest(const FString& Parameters)
{
	{
		FTravelFixture F;
		F.SignIn(Connecting::PlayerA);
		UFlockMultiplayerSession* Hosted = F.Hosted(*this);
		FNodes Nodes;
		FFlockTravelToSessionOptions Options;
		Options.HostLevel = TEXT("/Game/Arena");
		Nodes.Travel(UFlockTravelToSessionAction::TravelToSession(nullptr, Hosted, Options))->ActivateWithProviderForTesting(&*F.Provider);
		F.Game->Opens(7777);
		F.FramesUntil([&]() { return !Nodes->Fired.IsEmpty(); });
		TestEqualSensitive(TEXT("started as host: Joined"), Nodes->FiredText(), FString(TEXT("joined")));
		TestEqualSensitive(TEXT("its outcome"), Nodes->Travel.Outcome, UFlockMultiplayerLibrary::TravelOutcomeStartedAsHost());
	}
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.JoinedAsB(*this, F.Served);
		FNodes Nodes;
		UFlockTravelToSessionAction* Action = Nodes.Travel(UFlockTravelToSessionAction::TravelToSession(nullptr, Session, FFlockTravelToSessionOptions()));
		Action->ActivateWithProviderForTesting(&*F.Provider);
		Action->Cancel();
		F.Server->Publishes(F.Served, Connecting::Direct(Connecting::Text(TEXT("198.51.100.4")), Connecting::Number(7777), TEXT("192.168.1.20"),
			TEXT("198.51.100.4")));
		F.Advance(0.25);
		F.Frame();
		TestTrue(TEXT("cancelled: no pin"), Nodes->Fired.IsEmpty());
		TestEqual(TEXT("and no travel"), F.Game->TraveledTo.Num(), 0);
	}
	return true;
}

FLOCK_NODES_TEST(FFlockNodesTravelCallerTest, "Travel.CalledFromInsideALevel_Warns_FromTheGameInstanceItDoesNot")
bool FFlockNodesTravelCallerTest::RunTest(const FString& Parameters)
{
	FLevelWorld Level;
	UObject* InTheLevel = NewObject<UFlockMultiplayerNodeTestListener>(Level.World, TEXT("BP_MainMenu_C_0"));
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UObject* InTheInstance = NewObject<UFlockMultiplayerNodeTestListener>(Instance.Get());
	TestTrue(TEXT("a world"), UFlockTravelToSessionAction::IsGoneAfterTheLevelChanges(Level.World));
	TestTrue(TEXT("an object of the level"), UFlockTravelToSessionAction::IsGoneAfterTheLevelChanges(InTheLevel));
	TestFalse(TEXT("the game instance"), UFlockTravelToSessionAction::IsGoneAfterTheLevelChanges(Instance.Get()));
	TestFalse(TEXT("an object the game instance holds"), UFlockTravelToSessionAction::IsGoneAfterTheLevelChanges(InTheInstance));

	for (UObject* Caller : TArray<UObject*>{ InTheLevel, Instance.Get() })
	{
		FTravelFixture F;
		UFlockMultiplayerSession* Session = F.PublishedAndJoinedAsB(*this);
		FNodes Nodes;
		UFlockTravelToSessionAction* Action = Nodes.Travel(UFlockTravelToSessionAction::TravelToSession(Caller, Session, FFlockTravelToSessionOptions()));
		Action->ActivateWithProviderForTesting(&*F.Provider);
		Action->Cancel();
		const int32 Warned = F.WarningsContaining(TEXT("Call Flock Travel To Session from the Game Instance Blueprint"));
		if (Caller == InTheLevel)
		{
			TestEqual(TEXT("from the level: warned once, naming the caller and the fix"), Warned, 1);
			TestEqual(TEXT("naming the caller"), F.WarningsContaining(TEXT("called from BP_MainMenu_C_0")), 1);
		}
		else
		{
			TestEqual(TEXT("from the game instance: no warning"), Warned, 0);
		}
	}
	return true;
}

// ---- what every node shares ----

FLOCK_NODES_TEST(FFlockNodesOriginTest, "ANodesCalls_AreLoggedUnderItsCaller_InTheRequestsOwnLines")
bool FFlockNodesOriginTest::RunTest(const FString& Parameters)
{
	Matchmaking::FMatchmakingFixture F;
	F.SignIn(Matchmaking::PlayerA);
	FNodes Nodes;
	Nodes.Party(UFlockPartyAction::JoinParty(nullptr, TEXT("NOPE42")))->ActivateWithProviderForTesting(&*F.Provider);
	TestEqualSensitive(TEXT("precondition: the server refused the code"), Nodes->FiredText(), FString(TEXT("failure")));
	// The party requests' own line, not only the HTTP lines beneath it: the call went through another provider than the node's.
	TestTrue(TEXT("the request's failure names the node"), Nodes->Error.Operation.Len() > 0
		&& Matchmaking::Containing(F.Log->Warnings, Nodes->Error.Operation + TEXT(" [Blueprint node]")) + Matchmaking::Containing(F.Log->Errors,
			Nodes->Error.Operation + TEXT(" [Blueprint node]")) >= 1);
	TestEqual(TEXT("never as C++"), Matchmaking::Containing(F.Log->Warnings, TEXT("[C++]")) + Matchmaking::Containing(F.Log->Errors, TEXT("[C++]")), 0);
	TestEqualSensitive(TEXT("the origin restored after the call"), FFlockCallOrigin::Get(), FString(TEXT("C++")));
	return true;
}

FLOCK_NODES_TEST(FFlockNodesConstantsTest, "TheConstantNodes_SpellEachValueAsTheSdkDoes")
bool FFlockNodesConstantsTest::RunTest(const FString& Parameters)
{
	using L = UFlockMultiplayerLibrary;
	const TArray<TPair<FString, FString>> Pairs = {
		{ L::SessionEndReasonEndedByHost(), TEXT("ended_by_host") },
		{ L::SessionEndReasonHostLeft(), TEXT("host_left") },
		{ L::SessionEndReasonExpired(), TEXT("expired") },
		{ L::SessionEndReasonEmpty(), TEXT("empty") },
		{ L::SessionEndReasonLeft(), TEXT("left") },
		{ L::SessionEndReasonDropped(), TEXT("dropped") },
		{ L::SessionEndReasonMovedToAnotherSession(), TEXT("moved_to_another_session") },
		{ L::SessionEndReasonSignedOut(), TEXT("signed_out") },
		{ L::TravelOutcomeStartedAsHost(), TEXT("started_as_host") },
		{ L::TravelOutcomeConnected(), TEXT("connected") },
		{ L::TravelOutcomeHostNeverPublished(), TEXT("host_never_published") },
		{ L::TravelOutcomeSessionEnded(), TEXT("session_ended") },
		{ L::TravelOutcomeCouldNotConnect(), TEXT("could_not_connect") },
		{ L::TravelOutcomeCouldNotStart(), TEXT("could_not_start") },
		{ L::TravelOutcomeNotDirect(), TEXT("not_direct") },
		{ L::TravelOutcomeRefused(), TEXT("refused") },
		{ L::RefusedReasonNoJoinToken(), TEXT("no_join_token") },
		{ L::RefusedReasonJoinTokenInvalid(), TEXT("join_token_invalid") },
		{ L::RefusedReasonAlreadyConnected(), TEXT("already_connected") },
		{ L::RefusedReasonFlockUnreachable(), TEXT("flock_unreachable") },
		{ L::RefusedReasonSessionEnded(), TEXT("session_ended") },
		{ L::RefusedReasonNotHost(), TEXT("not_host") },
		{ L::RefusedReasonRefusedByTheGame(), TEXT("refused_by_the_game") },
		{ L::RefusedReasonJoinCheckMissing(), TEXT("join_check_missing") },
		{ L::MatchmakingOutcomeMatched(), TEXT("matched") },
		{ L::MatchmakingOutcomeExpired(), TEXT("expired") },
		{ L::MatchmakingOutcomePartyChanged(), TEXT("party_changed") },
		{ L::MatchmakingOutcomeCancelled(), TEXT("cancelled") },
		{ L::ConnectionModeDirect(), TEXT("direct") },
		{ L::DirectConnectionValueAddress(), TEXT("address") },
		{ L::DirectConnectionValuePort(), TEXT("port") },
		{ L::DirectConnectionValueLanAddress(), TEXT("lan_address") },
		{ L::DirectConnectionValuePublicAddress(), TEXT("public_address") },
	};
	for (const TPair<FString, FString>& Pair : Pairs)
	{
		TestEqualSensitive(*FString::Printf(TEXT("spelled as the server sends it: %s"), *Pair.Value), Pair.Key, Pair.Value);
	}
	// One node per spelling: the count of the library's string nodes, so a node added without its line here fails.
	int32 StringNodes = 0;
	for (TFieldIterator<UFunction> It(UFlockMultiplayerLibrary::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		const FStrProperty* Returned = CastField<FStrProperty>((*It)->GetReturnProperty());
		StringNodes += Returned != nullptr ? 1 : 0;
	}
	TestEqual(TEXT("every string node checked here"), StringNodes, Pairs.Num());
	FFlockMatchmakingResult Result;
	Result.Outcome = TEXT("Matched");
	TestFalse(TEXT("Is Matched reads the spelling letter for letter"), L::IsMatched(Result));
	return true;
}

FLOCK_NODES_TEST(FFlockNodesRunningGameTest, "TheNodesFindTheRunningGamesFlock")
bool FFlockNodesRunningGameTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World() != nullptr)
		{
			World = Context.World();
		}
	}
	if (World == nullptr || UFlockSubsystem::Get(World) == nullptr || UFlockSubsystem::Get(World)->GetMultiplayerProvider() == nullptr)
	{
		AddInfo(TEXT("No running game with Flock here (the editor's pass): the nodes' own lookup runs under -game."));
		return true;
	}
	// An empty code is refused by the running game's own Flock before anything is sent, so this reaches no server.
	FNodes Nodes;
	Nodes.Party(UFlockPartyAction::JoinParty(World, FString()))->Activate();
	TestEqualSensitive(TEXT("the failure pin"), Nodes->FiredText(), FString(TEXT("failure")));
	TestTrue(TEXT("Flock's own answer, not that Flock is not running"), Nodes->Error.Hint != FFlockErrorHints::InitializeFlockFirst()
		&& Nodes->Error.Type == EFlockErrorType::Validation);

	// Travel To Session warns through the running game's own logger, called from inside the level.
	FTravelFixture F;
	UFlockMultiplayerSession* Session = F.PublishedAndJoinedAsB(*this);
	UObject* InTheLevel = NewObject<UFlockMultiplayerNodeTestListener>(World, MakeUniqueObjectName(World, UFlockMultiplayerNodeTestListener::StaticClass(), TEXT("BP_LobbyWidget_C")));
	AddExpectedMessagePlain(TEXT("Travel To Session was called from BP_LobbyWidget_C"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FNodes Travel;
	UFlockTravelToSessionAction* Action = Travel.Travel(UFlockTravelToSessionAction::TravelToSession(InTheLevel, Session, FFlockTravelToSessionOptions()));
	Action->Activate();
	Action->Cancel();
	return true;
}

#endif // WITH_AUTOMATION_TESTS
