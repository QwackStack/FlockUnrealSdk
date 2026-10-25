// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Multiplayer/FlockJoinChecksOnHost.h"
#include "Multiplayer/Relay/FlockBridgeMessages.h"
#include "Multiplayer/Relay/FlockRelayBridge.h"
#include "Tests/Support/FlockJoinFixture.h"
#include "Tests/Support/FlockRelayFixture.h"
#include "Tests/Support/FlockSessionTestListener.h"
#include "Tests/Support/FlockTestRetries.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "UObject/StrongObjectPtr.h"

// Travel To Session through Flock's relay, over the session stack as the provider builds it, a fake relay on loopback and a
// scripted engine whose host side listens on a real loopback socket: the host's relay and bridge, players let in through it,
// a player reaching its host through a bridge of its own, the three relay choices, the try on the host's network before the
// relay, the seat's relay address, and a fresh join token for every travel back to the host.

#define FLOCK_RELAY_TRAVEL_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Relay.Travel." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockRelayTravelTestHelpers
{
	using namespace FlockJoinVerificationTestHelpers;
	using FEngineSocket = FlockRelayTestHelpers::FEngineSocket;

	inline FFlockTravelToSessionOptions Relaying(EFlockRelayUse Use)
	{
		FFlockTravelToSessionOptions Options;
		Options.Relay = Use;
		Options.WaitForHostSeconds = 30.f;
		return Options;
	}

	struct FRelayTravelFixture : FJoinFixture
	{
		TUniquePtr<FFlockFakeRelayServer> Relay = MakeUnique<FFlockFakeRelayServer>();
		/** Other devices' relay addresses: a host this game joins, or players of this game's host. */
		TArray<TSharedRef<FFlockRelayConnection>> Devices;
		/** The net driver of this game as host: a real loopback socket its level "listens" on. */
		FEngineSocket Engine;

		explicit FRelayTravelFixture(int32 MaxRetries = 0)
			: FJoinFixture(MaxRetries)
		{
			Server->RelayUrls.Add(Relay->Url());
			Server->RelayPassword = Relay->Password;
			Sessions().SetRelayTimingForTesting(FlockRelayTestHelpers::Quick());
		}

		~FRelayTravelFixture()
		{
			for (const TSharedRef<FFlockRelayConnection>& Device : Devices)
			{
				Device->Close();
			}
			Devices.Reset();
			FFlockRelayBridge::MostPlayersForTesting = 0;
			FFlockRelayBridge::NotLetInWithinForTesting = 0.0;
		}

		FString RelayServer() const { return FString::Printf(TEXT("127.0.0.1:%d"), Relay->GetPort()); }

		/** One frame of this game and of every other device's relay. */
		void RelayFrame()
		{
			Frame();
			const TArray<TSharedRef<FFlockRelayConnection>> Each = Devices;
			for (const TSharedRef<FFlockRelayConnection>& Device : Each)
			{
				Device->Tick();
			}
		}

		/** Runs frames, a millisecond apart in real time for the loopback to carry packets, until Done or Seconds pass. */
		bool Until(TFunctionRef<bool()> Done, double Seconds = 5.0)
		{
			const double Ends = FPlatformTime::Seconds() + Seconds;
			while (!Done() && FPlatformTime::Seconds() < Ends)
			{
				RelayFrame();
				FPlatformProcess::Sleep(0.001f);
			}
			RelayFrame();
			return Done();
		}

		/** Another device's address on the relay, opened to OpenTo (its own address when empty). */
		TSharedPtr<FFlockRelayConnection> Device(FAutomationTestBase& Test, const FString& Player, const FFlockRelayPeer& OpenTo = FFlockRelayPeer())
		{
			FFlockRelayLogin Login;
			Login.Host = TEXT("127.0.0.1");
			Login.Port = Relay->GetPort();
			Login.Username = TEXT("1760000000:game:") + Player;
			Login.Password = Relay->Password;
			const TSharedRef<FlockRelayTestHelpers::FOpening> Opening = MakeShared<FlockRelayTestHelpers::FOpening>();
			Devices.Add(FFlockRelayConnection::Open(Login, TArray<FFlockRelayStunLookUp>(), FlockRelayTestHelpers::Quick(), Log, FlockRelayTestHelpers::Into(Opening)));
			Until([&Opening]() { return Opening->bAnswered; });
			if (!Test.TestTrue(FString::Printf(TEXT("precondition: %s's relay opened"), *Player), Opening->Result.bSuccess))
			{
				return nullptr;
			}
			const TSharedPtr<FFlockRelayConnection> Opened = Opening->Result.Value;
			bool bOpen = false;
			bool bAnswered = false;
			Opened->OpenTo(OpenTo.IsEmpty() ? Opened->GetAddress() : OpenTo, [&bOpen, &bAnswered](TFlockRelayResult<bool> Result)
			{
				bAnswered = true;
				bOpen = Result.bSuccess;
			});
			Until([&bAnswered]() { return bAnswered; });
			return Test.TestTrue(TEXT("precondition: opened"), bOpen) ? Opened : nullptr;
		}

		/** What a host publishes through Travel To Session: its relay address and server, and its direct addresses when given. */
		TSharedRef<FJsonObject> RelayConnectionOf(const FFlockRelayPeer& HostAddress, const FString& OnServer, bool bWithDirect = false,
			const FString& Lan = TEXT("192.168.1.20"), const FString& Public = HostsPublicAddress) const
		{
			const TSharedRef<FJsonObject> Connection = bWithDirect ? Direct(Text(Public.IsEmpty() ? Lan : Public), Number(7777), Lan, Public)
				: MakeShared<FJsonObject>();
			Connection->SetStringField(TEXT("mode"), TEXT("relay"));
			Connection->SetStringField(TEXT("relay_address"), HostAddress.ToString());
			Connection->SetStringField(TEXT("relay_server"), OnServer);
			return Connection;
		}

		/** This game as B in A's session, A having published Connection; the session. */
		UFlockMultiplayerSession* PlayerOf(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Connection)
		{
			UFlockMultiplayerSession* Joined = JoinedAsB(Test, Served);
			Server->Publishes(Served, Connection);
			// The session read once, so the player holds the host's connection, as its connection wait would bring it.
			if (Joined != nullptr)
			{
				Provider->GetSession(Joined->GetId(), [](TFlockResult<UFlockMultiplayerSession*>) {});
			}
			return Joined;
		}

		/** This game (A) hosting through Travel To Session with Options, its level listening on the engine socket; the travel's answer. */
		TSharedRef<FTravelAnswer> HostingWith(FAutomationTestBase& Test, const FFlockTravelToSessionOptions& Options, UFlockMultiplayerSession*& OutSession)
		{
			SignIn(PlayerA);
			OutSession = Hosted(Test);
			const TSharedRef<FTravelAnswer> Answer = MakeShared<FTravelAnswer>();
			if (OutSession == nullptr)
			{
				return Answer;
			}
			Served = Server->ById(OutSession->GetId()).ToSharedRef();
			Game->OwnPlayers.Add(HostsOwnController());
			const TSharedRef<FTravelAnswer> Travelling = Travel(OutSession, Options);
			Game->Opens(Engine.Port);
			Until([&Travelling]() { return Travelling->bAnswered; });
			return Travelling;
		}

		FFlockRelayBridge::FPtr HostsBridge() const { return Provider->GetJoinChecks().GetRelayBridge(); }

		/** The session read once, as the game's own reads bring it. */
		void ReadsTheSession(UFlockMultiplayerSession* Read)
		{
			bool bRead = false;
			if (Read != nullptr)
			{
				Provider->GetSession(Read->GetId(), [&bRead](TFlockResult<UFlockMultiplayerSession*>) { bRead = true; });
			}
			Until([&bRead]() { return bRead; }, 2.0);
		}

		TSharedPtr<FFlockRelayConnection> SessionsRelay() { return Sessions().GetRelayForTesting(); }

		/** A player's device sends Text to this host's relay address; the loopback port it reached this host's engine from, or 0. */
		int32 ReachesTheHost(const TSharedPtr<FFlockRelayConnection>& From, const FString& Text)
		{
			const TSharedPtr<FFlockRelayConnection> Hosts = SessionsRelay();
			if (!Hosts.IsValid())
			{
				return 0;
			}
			const TArray<uint8> Payload = FlockRelayTestHelpers::Bytes(Text);
			From->Send(Hosts->GetAddress(), Payload.GetData(), Payload.Num());
			Until([&]()
			{
				Engine.ReadEverything();
				return Engine.PortOf(Text) != 0;
			}, 3.0);
			return Engine.PortOf(Text);
		}

		/** The connection B's game makes through its relay address, let in or not by this host: its join answer. */
		FJoining JoinsThroughTheRelay(const FString& Token, int32 LoopbackPort)
		{
			return Joins(WithToken(Token), nullptr, nullptr, TOptional<FString>(), FFlockSockets::LoopbackIpv4, LoopbackPort);
		}

		/** Relay logins asked for a session: the route without a session reads Flock's STUN servers alone. */
		int32 RelayLoginsAsked() const
		{
			return Server->RequestsTo(ERoute::RelayCredentials).FilterByPredicate([](const FFlockHttpRequest& Request)
			{
				const TSharedPtr<FJsonObject> Body = BodyOf(Request);
				return Body.IsValid() && Body->HasField(TEXT("session_id"));
			}).Num();
		}

		int32 InfosContaining(const FString& Needle) const
		{
			return Log->Infos.FilterByPredicate([&](const FString& Line) { return Line.Contains(Needle); }).Num();
		}
	};
}

using namespace FlockRelayTravelTestHelpers;

// ---- the host ----

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelAlwaysHostTest, "Host.AnAlwaysHost_PublishesItsRelayAddressAlone_AndTakesPlayersThroughItsBridge")
bool FFlockRelayTravelAlwaysHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session);
	if (!TestTrue(TEXT("hosting"), Hosting->Succeeded()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("started as host"), Hosting->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	TestTrue(TEXT("through the relay"), Hosting->Result.Value.bThroughRelay);
	TestTrue(TEXT("no address of its own published"), Hosting->Result.Value.Address.IsEmpty());
	const TSharedPtr<FJsonObject> Published = F.LastPublished();
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	if (!TestTrue(TEXT("the session's relay"), Relay.IsValid() && Published.IsValid()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("mode relay"), F.Field(Published, TEXT("mode")), FString(TEXT("relay")));
	TestEqualSensitive(TEXT("its relay address"), F.Field(Published, TEXT("relay_address")), Relay->GetAddress().ToString());
	TestEqualSensitive(TEXT("the relay server it is on"), F.Field(Published, TEXT("relay_server")), F.RelayServer());
	TestFalse(TEXT("no direct address"), Published->HasField(TEXT("address")) || Published->HasField(TEXT("lan_address")) || Published->HasField(TEXT("port")));
	TestTrue(TEXT("opened to its own relay IP, which every address of a one-IP relay shares"), Relay->IsOpenTo(Relay->GetAddress()));
	TestTrue(TEXT("the hosting holds a bridge"), F.HostsBridge().IsValid());
	TestTrue(TEXT("which hears what arrives"), Relay->HandsArrivalsOver());
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	TestTrue(TEXT("a player through the relay reaches the host's net driver from a loopback port"), Player.IsValid() && F.ReachesTheHost(Player, TEXT("handshake")) > 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelWhenNeededHostTest, "Host.AWhenNeededHost_PublishesItsRelayAddress_AndItsDirectOnes")
bool FFlockRelayTravelWhenNeededHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, FFlockTravelToSessionOptions(), Session);
	if (!TestTrue(TEXT("hosting"), Hosting->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Published = F.LastPublished();
	TestEqualSensitive(TEXT("mode relay"), F.Field(Published, TEXT("mode")), FString(TEXT("relay")));
	TestFalse(TEXT("its relay address"), F.Field(Published, TEXT("relay_address")).IsEmpty());
	TestEqualSensitive(TEXT("its public address too"), F.Field(Published, TEXT("public_address")), HostsPublicAddress);
	TestFalse(TEXT("and its LAN address"), F.Field(Published, TEXT("lan_address")).IsEmpty());
	double Port = 0;
	TestTrue(TEXT("on the port it listens on"), Published.IsValid() && Published->TryGetNumberField(TEXT("port"), Port) && static_cast<int32>(Port) == F.Engine.Port);
	TestTrue(TEXT("through the relay"), Hosting->Result.Value.bThroughRelay);
	TestEqualSensitive(TEXT("its published address"), Hosting->Result.Value.Address, HostsPublicAddress);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelNeverHostTest, "Host.ANeverHost_OpensNoRelay_AndPublishesDirectly")
bool FFlockRelayTravelNeverHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, Relaying(EFlockRelayUse::Never), Session);
	TestTrue(TEXT("hosting"), Hosting->Succeeded());
	TestEqualSensitive(TEXT("mode direct"), F.Field(F.LastPublished(), TEXT("mode")), FString(TEXT("direct")));
	TestEqual(TEXT("no relay logins asked"), F.RelayLoginsAsked(), 0);
	TestFalse(TEXT("not through the relay"), Hosting->Result.Value.bThroughRelay);
	TestFalse(TEXT("no bridge"), F.HostsBridge().IsValid());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSwitchedOffTest, "Host.AHostWhoseRelayIsSwitchedOff_PublishesDirectly_WithoutAWarning")
bool FFlockRelayTravelSwitchedOffTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.Server->RelayUrls.Reset();
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, FFlockTravelToSessionOptions(), Session);
	TestTrue(TEXT("hosting"), Hosting->Succeeded());
	TestEqualSensitive(TEXT("mode direct"), F.Field(F.LastPublished(), TEXT("mode")), FString(TEXT("direct")));
	TestEqualSensitive(TEXT("why it is direct"), Hosting->Result.Value.RelayFailureReason, FString(FlockRelayFailureReasons::NotOffered));
	TestFalse(TEXT("not through the relay"), Hosting->Result.Value.bThroughRelay);
	TestEqual(TEXT("the studio's choice is not warned about"), F.WarningsContaining(TEXT("relay")), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelPausedTest, "Host.AHostWhoseRelayIsPaused_PublishesDirectly_AndSaysWhy")
bool FFlockRelayTravelPausedTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.Server->bRelayPaused = true;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, FFlockTravelToSessionOptions(), Session);
	TestTrue(TEXT("hosting"), Hosting->Succeeded());
	TestEqualSensitive(TEXT("mode direct"), F.Field(F.LastPublished(), TEXT("mode")), FString(TEXT("direct")));
	TestEqualSensitive(TEXT("why"), Hosting->Result.Value.RelayFailureReason, FString(FlockRelayFailureReasons::Paused));
	TestEqual(TEXT("said once"), F.WarningsContaining(TEXT("cannot join through Flock's relay")), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelAlwaysFailsTest, "Host.AnAlwaysHostWhoseRelayFails_StopsHosting_AndPublishesNothing")
bool FFlockRelayTravelAlwaysFailsTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.Server->RelayUrls.Reset();
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session);
	TestTrue(TEXT("answered"), Hosting->Succeeded());
	TestEqualSensitive(TEXT("relay_failed"), Hosting->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::RelayFailed));
	TestEqualSensitive(TEXT("why"), Hosting->Result.Value.RelayFailureReason, FString(FlockRelayFailureReasons::NotOffered));
	TestEqual(TEXT("the level stopped listening"), F.Game->StoppedListening, 1);
	TestEqual(TEXT("nothing published"), F.Server->Count(ERoute::Publish), 0);
	TestEqual(TEXT("said why it stopped"), F.WarningsContaining(TEXT("Stopped hosting")), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelStoppedWhileOpeningTest, "Host.AHostWhoseLevelStopsListeningWhileTheRelayOpens_PublishesNothing")
bool FFlockRelayTravelStoppedWhileOpeningTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Logins = F.Server->HoldNext(ERoute::RelayCredentials);
	const TSharedRef<FTravelAnswer> Hosting = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Game->Opens(F.Engine.Port);
	F.RelayFrame();
	TestTrue(TEXT("precondition: the relay's logins asked"), Logins->bArrived);
	// The game stops its own level while the relay opens.
	F.Game->StopListening();
	Logins->Release();
	F.Until([&]() { return Hosting->bAnswered; });
	TestTrue(TEXT("answered"), Hosting->Succeeded());
	TestEqualSensitive(TEXT("could_not_start"), Hosting->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotStart));
	TestEqual(TEXT("a host that is gone is not published"), F.Server->Count(ERoute::Publish), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelCancelWhileOpeningTest, "Host.CancellingWhileTheHostsRelayOpens_StopsListening_AndPublishesNothing")
bool FFlockRelayTravelCancelWhileOpeningTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Logins = F.Server->HoldNext(ERoute::RelayCredentials);
	FFlockRequestHandle Handle;
	const TSharedRef<FTravelAnswer> Hosting = F.Travel(Session, FFlockTravelToSessionOptions(), &Handle);
	F.Game->Opens(F.Engine.Port);
	F.RelayFrame();
	if (!TestTrue(TEXT("precondition: the relay's logins asked"), Logins->bArrived))
	{
		return false;
	}
	Handle.Cancel();
	TestTrue(TEXT("the cancel is the game's"), Hosting->FailedAs(EFlockErrorType::Cancelled));
	TestEqual(TEXT("nobody waits on a host the game gave up on: its level stops listening"), F.Game->StoppedListening, 1);
	Logins->Release();
	F.Until([]() { return false; }, 0.3);
	TestEqual(TEXT("nothing published"), F.Server->Count(ERoute::Publish), 0);
	TestFalse(TEXT("no bridge"), F.HostsBridge().IsValid());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelOpeningRefusedTest, "Host.AHostWhoseRelayOpeningIsRefused_GivesTheRelayBack_AndPublishesDirectly")
bool FFlockRelayTravelOpeningRefusedTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.Relay->RefusedOpenings.Add(FFlockFakeRelayServer::RelayIp);
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, FFlockTravelToSessionOptions(), Session);
	TestTrue(TEXT("hosting"), Hosting->Succeeded());
	TestEqualSensitive(TEXT("why"), Hosting->Result.Value.RelayFailureReason, FString(FlockRelayFailureReasons::OpenRefused));
	TestEqualSensitive(TEXT("mode direct"), F.Field(F.LastPublished(), TEXT("mode")), FString(TEXT("direct")));
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	TestTrue(TEXT("a relay address no player can reach is given back"), !Relay.IsValid() || Relay->HasStopped());
	TestFalse(TEXT("no bridge"), F.HostsBridge().IsValid());
	F.Until([]() { return false; }, 0.2);
	TestFalse(TEXT("a relay the SDK gave back is not told lost"), F.Sessions().WasRelayLostToldForTesting());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRelayOnlyTest, "Host.AnAlwaysHost_TurnsAwayAPlayerWhoArrivesDirectly_WithoutAskingFlock")
bool FFlockRelayTravelRelayOnlyTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining Direct = F.Joins(WithToken(Token));
	TestEqualSensitive(TEXT("relay_only"), Direct.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::RelayOnly));
	TestEqual(TEXT("Flock was not asked"), F.Server->Count(ERoute::Verify), 0);
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, F.SessionsRelay()->GetAddress());
	const int32 Port = Player.IsValid() ? F.ReachesTheHost(Player, TEXT("handshake")) : 0;
	const FJoining Relayed = F.JoinsThroughTheRelay(F.Server->MintsJoinToken(F.Served, PlayerB), Port);
	TestTrue(TEXT("through the relay, let in"), Relayed.Answer->LetIn());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelLetInTest, "Host.ARelayedPlayer_IsLetIn_KnownByItsConnection_AndGivenAChannel")
bool FFlockRelayTravelLetInTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	const int32 Port = Player.IsValid() ? F.ReachesTheHost(Player, TEXT("handshake")) : 0;
	if (!TestTrue(TEXT("precondition: the player's packets reach the engine"), Port > 0))
	{
		return false;
	}
	TestFalse(TEXT("no channel before the check lets it in"), Relay->KeepsChannelToForTesting(Player->GetAddress()));
	const FJoining Joining = F.JoinsThroughTheRelay(F.SeatedWithToken(PlayerB), Port);
	TestTrue(TEXT("let in"), Joining.Answer->LetIn());
	TestEqualSensitive(TEXT("the game's check saw the loopback address"), F.GameAsked.Num() > 0 ? F.GameAsked.Last().Address : FString(),
		FString(FFlockSockets::LoopbackIpv4));
	TestTrue(TEXT("its packets ride on a channel now"), Relay->KeepsChannelToForTesting(Player->GetAddress()));
	F.PlaysThrough(Joining.Connection);
	TestEqualSensitive(TEXT("known by its connection"), Session->PlayerForConnection(JoinedController()), PlayerB);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSilentEarlierTest, "Host.ARelayedPlayersSilentEarlierConnection_GivesWay_AndItsOldRelayAddressIsForgotten")
bool FFlockRelayTravelSilentEarlierTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	// B's game, then B's game again on a new relay address (its first relay was lost).
	const TSharedPtr<FFlockRelayConnection> Lost = F.Device(*this, PlayerB, Relay->GetAddress());
	const int32 LostPort = Lost.IsValid() ? F.ReachesTheHost(Lost, TEXT("first")) : 0;
	const FString Token = F.SeatedWithToken(PlayerB);
	const FJoining First = F.JoinsThroughTheRelay(Token, LostPort);
	if (!TestTrue(TEXT("precondition: the first let in"), First.Answer->LetIn()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Again = F.Device(*this, PlayerB, Relay->GetAddress());
	const int32 AgainPort = Again.IsValid() ? F.ReachesTheHost(Again, TEXT("second")) : 0;
	if (!TestTrue(TEXT("precondition: a loopback port of its own"), AgainPort > 0 && AgainPort != LostPort))
	{
		return false;
	}
	F.Game->SilentFor.Add(First.Connection, 6.0);
	const FJoining Newer = F.JoinsThroughTheRelay(F.SeatedWithToken(PlayerB), AgainPort);
	TestTrue(TEXT("the newer let in"), Newer.Answer->LetIn());
	TestTrue(TEXT("the earlier closed"), F.Game->Closed.Contains(First.Connection));
	F.RelayFrame();
	TestEqual(TEXT("its old relay address forgotten"), F.HostsBridge()->LoopbackPortOf(Lost->GetAddress()), 0);
	TestFalse(TEXT("its channel let go"), Relay->KeepsChannelToForTesting(Lost->GetAddress()));
	TestEqual(TEXT("the newer keeps its place"), F.HostsBridge()->LoopbackPortOf(Again->GetAddress()), AgainPort);
	TestTrue(TEXT("on a channel"), Relay->KeepsChannelToForTesting(Again->GetAddress()));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelOutsiderTest, "Host.AnOutsiderOnTheRelay_IsRefused_AndGetsNoChannel")
bool FFlockRelayTravelOutsiderTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Outsider = F.Device(*this, TEXT("player-outsider"), Relay->GetAddress());
	const int32 Port = Outsider.IsValid() ? F.ReachesTheHost(Outsider, TEXT("handshake")) : 0;
	const FJoining Joining = F.JoinsThroughTheRelay(TEXT("made-up"), Port);
	TestEqualSensitive(TEXT("refused by Flock's verdict"), Joining.Answer->RefusedFor(), FString(FlockTravelToSessionRefusedReasons::JoinTokenInvalid));
	TestFalse(TEXT("no channel for it"), Relay->KeepsChannelToForTesting(Outsider->GetAddress()));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelLeavesTest, "Host.ARelayedPlayerWhoLeaves_IsForgottenByTheBridge")
bool FFlockRelayTravelLeavesTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	const int32 Port = Player.IsValid() ? F.ReachesTheHost(Player, TEXT("handshake")) : 0;
	const FJoining Joining = F.JoinsThroughTheRelay(F.SeatedWithToken(PlayerB), Port);
	if (!TestTrue(TEXT("precondition: let in"), Joining.Answer->LetIn()))
	{
		return false;
	}
	F.Game->Leaves(Joining.Connection);
	F.RelayFrame();
	TestEqual(TEXT("its place given back"), F.HostsBridge()->LoopbackPortOf(Player->GetAddress()), 0);
	TestFalse(TEXT("its channel let go"), Relay->KeepsChannelToForTesting(Player->GetAddress()));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReconnectKeepsPathTest, "Host.AnOldConnectionLeaving_WhileTheNextOnTheSameRelayAddressIsChecked_LeavesTheNextItsPath")
bool FFlockRelayTravelReconnectKeepsPathTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	const int32 Port = Player.IsValid() ? F.ReachesTheHost(Player, TEXT("handshake")) : 0;
	const FJoining Old = F.JoinsThroughTheRelay(F.SeatedWithToken(PlayerB), Port);
	if (!TestTrue(TEXT("precondition: let in"), Old.Answer->LetIn()))
	{
		return false;
	}
	// The player's game connects again through the same relay address while the host still checks it (a level change, measured).
	const TSharedRef<FFlockFakePartyServer::FHeld> Verifying = F.Server->HoldNext(ERoute::Verify);
	const FJoining Next = F.JoinsThroughTheRelay(F.Server->MintsJoinToken(F.Served, PlayerB), Port);
	F.Game->Leaves(Old.Connection);
	F.RelayFrame();
	TestEqual(TEXT("the address keeps its loopback port for the next connection"), F.HostsBridge()->LoopbackPortOf(Player->GetAddress()), Port);
	TestFalse(TEXT("its channel waits until the next is let in"), Relay->KeepsChannelToForTesting(Player->GetAddress()));
	Verifying->Release();
	F.RelayFrame();
	TestTrue(TEXT("the next is let in"), Next.Answer->LetIn());
	TestTrue(TEXT("and gets the channel"), Relay->KeepsChannelToForTesting(Player->GetAddress()));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelStopListeningTest, "Host.StoppingTheHostsLevel_ClosesItsBridge_AndTheRelayStaysTheSessions")
bool FFlockRelayTravelStopListeningTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.HostsBridge();
	F.Game->StopListening();
	F.RelayFrame();
	TestTrue(TEXT("the bridge closed with the hosting"), Bridge.IsValid() && Bridge->IsClosed());
	TestFalse(TEXT("no hosting holds one"), F.HostsBridge().IsValid());
	TestTrue(TEXT("the relay stays the session's"), F.SessionsRelay().IsValid() && F.SessionsRelay()->IsOpen());
	TestFalse(TEXT("its arrivals handed back"), F.SessionsRelay()->HandsArrivalsOver());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSessionEndTest, "Host.TheSessionsEnd_ClosesTheHostsBridge_AndItsRelay")
bool FFlockRelayTravelSessionEndTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.HostsBridge();
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedRef<FSessionAnswer> Ended = MakeShared<FSessionAnswer>();
	Session->End(Into(Ended));
	F.RelayFrame();
	TestTrue(TEXT("ended"), Session->HasEnded());
	TestTrue(TEXT("the relay given back"), Relay.IsValid() && Relay->HasStopped());
	TestTrue(TEXT("the bridge closed"), Bridge.IsValid() && Bridge->IsClosed());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelShutdownTest, "Host.FlockShuttingDown_ClosesTheHostsBridge_ThoughItsLevelStaysOpen")
bool FFlockRelayTravelShutdownTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.HostsBridge();
	F.Provider->Shutdown();
	TestTrue(TEXT("no relay thread outlives Flock: the bridge closed"), Bridge.IsValid() && Bridge->IsClosed());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelHostFollowsTest, "Host.AHostsBridge_FollowsItsLevelToTheNextPortItListensOn")
bool FFlockRelayTravelHostFollowsTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	// The host's own level change: its level opens again, here on another port.
	FEngineSocket NextLevel;
	F.Game->State.ListeningPort = NextLevel.Port;
	F.RelayFrame();
	TestEqual(TEXT("the bridge sends to the port listening now"), F.HostsBridge()->GetNetDriverPortForTesting(), NextLevel.Port);
	F.Game->State.ListeningPort = 0;
	F.Game->State.bChangingLevel = true;
	F.RelayFrame();
	TestEqual(TEXT("a level still opening listens on nothing yet: the port is kept"), F.HostsBridge()->GetNetDriverPortForTesting(), NextLevel.Port);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatOpeningTest, "Host.AHost_LetsInAPlayerOnAnotherRelayIp_OnlyOnceThePlayersSeatSaysWhere")
bool FFlockRelayTravelSeatOpeningTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	// A relay that gives each address an IP of its own, as Cloudflare's did (measured).
	F.Relay->RelayIps = { TEXT("203.0.113.50"), TEXT("203.0.113.51") };
	F.Sessions().SetRelayedPlayersReadSecondsForTesting(2.0);
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	if (!Player.IsValid() || !TestNotEqual(TEXT("precondition: the player on another relay IP"), Player->GetAddress().Address, Relay->GetAddress().Address))
	{
		return false;
	}
	F.Server->Joins(F.Served, PlayerB);
	TestEqual(TEXT("not let in before its seat says where"), F.ReachesTheHost(Player, TEXT("too early")), 0);
	TestTrue(TEXT("the host reads the session often while it takes players through its relay"), F.Sessions().GetRelayedPlayersReadForTesting().IsValid()
		&& F.Sessions().GetRelayedPlayersReadForTesting()->IsRunning());
	F.Server->SeatSaysRelayAddress(F.Served, PlayerB, Player->GetAddress().ToString());
	F.Advance(2.0);
	F.Until([&]() { return Relay->IsOpenTo(Player->GetAddress()); }, 3.0);
	TestTrue(TEXT("opened to the player's IP from the next reading"), Relay->IsOpenTo(Player->GetAddress()));
	TestTrue(TEXT("its packets reach the host now"), F.ReachesTheHost(Player, TEXT("in time")) > 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatRefusedTest, "Host.AHostWhoseRelayRefusesAPlayersIp_SaysSoOnce_HoweverOftenItReads")
bool FFlockRelayTravelSeatRefusedTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.Relay->RelayIps = { TEXT("203.0.113.50"), TEXT("203.0.113.51") };
	F.Relay->RefusedOpenings.Add(TEXT("203.0.113.51"));
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	F.Server->Joins(F.Served, PlayerB);
	F.Server->SeatSaysRelayAddress(F.Served, PlayerB, TEXT("203.0.113.51:50000"));
	for (int32 Reading = 0; Reading < 4; ++Reading)
	{
		F.Advance(2.0);
		F.Until([]() { return false; }, 0.3);
	}
	TestEqual(TEXT("told once"), F.WarningsContaining(TEXT("cannot be let in")), 1);
	TestTrue(TEXT("remembered as refused"), F.SessionsRelay()->RefusedToOpenTo(FFlockRelayPeer(0xCB007133, 50000)));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReadOnlyWhileRelayingTest, "Host.AHost_ReadsTheSessionOften_OnlyWhileItTakesPlayersThroughItsRelay")
bool FFlockRelayTravelReadOnlyWhileRelayingTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Never), Session)->Succeeded()))
	{
		return false;
	}
	F.RelayFrame();
	TestFalse(TEXT("a host taking nobody through a relay reads no more than its heartbeats"), F.Sessions().GetRelayedPlayersReadForTesting().IsValid());
	FRelayTravelFixture G;
	UFlockMultiplayerSession* Relayed = nullptr;
	if (!TestTrue(TEXT("hosting through the relay"), G.HostingWith(*this, Relaying(EFlockRelayUse::Always), Relayed)->Succeeded()))
	{
		return false;
	}
	G.RelayFrame();
	TestTrue(TEXT("one that takes players through its relay reads often"), G.Sessions().GetRelayedPlayersReadForTesting().IsValid());
	const int32 ReadsBefore = G.Server->Count(ERoute::Read);
	G.Advance(2.0);
	G.Advance(2.0);
	TestTrue(TEXT("every 2 s"), G.Server->Count(ERoute::Read) >= ReadsBefore + 2);
	G.Game->StopListening();
	G.RelayFrame();
	G.RelayFrame();
	TestFalse(TEXT("and stops once its bridge closed"), G.Sessions().GetRelayedPlayersReadForTesting().IsValid());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReadRefusedTest, "Host.AHostWhoseOftenReadIsRefused_DoesNotSendItAgainEveryFrame")
bool FFlockRelayTravelReadRefusedTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::Read, FFlockFakeSessionServer::Refused(403, TEXT("request.forbidden")));
	F.Advance(2.0);
	F.RelayFrame();
	const int32 Reads = F.Server->Count(ERoute::Read);
	for (int32 Index = 0; Index < 5; ++Index)
	{
		F.Advance(2.0);
	}
	TestEqual(TEXT("not sent again"), F.Server->Count(ERoute::Read), Reads);
	TestEqual(TEXT("said once"), F.WarningsContaining(TEXT("no longer read every 2 s")), 1);
	return true;
}

// ---- a player ----

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelPlayerThroughTest, "Player.APlayer_ConnectsThroughTheRelay_ToARelayedHost")
bool FFlockRelayTravelPlayerThroughTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0 || Travel->bAnswered; });
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	if (!TestTrue(TEXT("travelling to a bridge"), Bridge.IsValid() && F.Game->TraveledTo.Num() == 1))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the engine travels to the bridge on the loopback"), F.Game->TraveledTo[0], FString::Printf(TEXT("127.0.0.1:%d"), Bridge->GetLocalPort()));
	TestFalse(TEXT("with its join token"), F.Game->TokensTraveledWith[0].IsEmpty());
	FEngineSocket ClientDriver;
	ClientDriver.SendTo(Bridge->GetLocalPort(), TEXT("hello host"));
	bool bHeard = false;
	F.Until([&]()
	{
		uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
		int32 Count = 0;
		FFlockRelayPeer From;
		while (!bHeard && Host->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From))
		{
			bHeard = FFlockRelayWire::FromUtf8(Buffer, Count) == TEXT("hello host");
		}
		return bHeard;
	}, 3.0);
	TestTrue(TEXT("what the engine sends reaches the host's relay address"), bHeard);
	F.Game->Arrives(true);
	F.RelayFrame();
	TestTrue(TEXT("connected"), Travel->Succeeded() && Travel->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive));
	TestTrue(TEXT("through the relay"), Travel->Result.Value.bThroughRelay);
	TestEqualSensitive(TEXT("the host's relay IP"), Travel->Result.Value.Address, Host->GetAddress().Ip());
	TestEqual(TEXT("and port"), Travel->Result.Value.Port, Host->GetAddress().Port);
	TestTrue(TEXT("the player stays with its host"), F.Sessions().GetTravels().IsWithTheHostForTesting());
	TestFalse(TEXT("its bridge kept"), Bridge->IsClosed());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatPublishTest, "Player.ARelayedPlayer_PublishesItsRelayAddressForItsSeat_BeforeConnecting")
bool FFlockRelayTravelSeatPublishTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	const TArray<FFlockHttpRequest> Seat = F.Server->RequestsTo(ERoute::PublishSeat);
	if (!TestEqual(TEXT("published once"), Seat.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Body = BodyOf(Seat[0]);
	const TSharedPtr<FJsonObject>* Connection = nullptr;
	TestTrue(TEXT("a connection"), Body.IsValid() && Body->TryGetObjectField(TEXT("connection_info"), Connection) && Connection != nullptr);
	TestEqualSensitive(TEXT("mode relay"), F.Field(*Connection, TEXT("mode")), FString(TEXT("relay")));
	TestEqualSensitive(TEXT("its relay address"), F.Field(*Connection, TEXT("relay_address")), F.SessionsRelay()->GetAddress().ToString());
	int32 SeatAt = INDEX_NONE;
	int32 TokenAt = INDEX_NONE;
	for (int32 Index = 0; Index < F.Server->Requests.Num(); ++Index)
	{
		const ERoute Route = FFlockFakeSessionServer::RouteOf(F.Server->Requests[Index]);
		SeatAt = Route == ERoute::PublishSeat && SeatAt == INDEX_NONE ? Index : SeatAt;
		TokenAt = Route == ERoute::JoinToken ? Index : TokenAt;
	}
	TestTrue(TEXT("before connecting"), SeatAt != INDEX_NONE && SeatAt < TokenAt);
	const FFlockFakeSessionServer::FSeat* Seated = F.Served->Seats.FindByPredicate([](const FFlockFakeSessionServer::FSeat& Each) { return Each.PlayerId == PlayerB; });
	TestEqualSensitive(TEXT("its seat says it now"), Seated != nullptr ? F.Field(Seated->Connection, TEXT("relay_address")) : FString(),
		F.SessionsRelay()->GetAddress().ToString());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatNotTakenTest, "Player.APlayerWhoseRelayAddressFlockWillNotTake_StillConnects_AndSaysWhy")
bool FFlockRelayTravelSeatNotTakenTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Server->AnswerNext(ERoute::PublishSeat, FFlockFakeSessionServer::Refused(404, TEXT("request.not_found")));
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqual(TEXT("travelled anyway"), F.Game->TraveledTo.Num(), 1);
	TestEqual(TEXT("said why"), F.WarningsContaining(TEXT("relay address could not be given to the host")), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatOnceTest, "Player.APlayerWhoseRelayAddressFlockCannotTakeNow_AsksOnce_AndConnects")
bool FFlockRelayTravelSeatOnceTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F(/*MaxRetries*/ 2);
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Server->AnswerNext(ERoute::PublishSeat, FFlockFakeSessionServer::Refused(503, TEXT("service.unavailable")));
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]()
	{
		FlockTestRetries::Pump();
		return F.Game->TraveledTo.Num() > 0;
	});
	TestEqual(TEXT("asked once, never again"), F.Server->Count(ERoute::PublishSeat), 1);
	TestEqual(TEXT("and connected"), F.Game->TraveledTo.Num(), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelOnTheHostsServerTest, "Player.APlayer_ReservesOnTheRelayServerTheHostUses")
bool FFlockRelayTravelOnTheHostsServerTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	// Flock lists a relay nobody answers first; the host is on the second.
	F.Server->RelayUrls.Insert(TEXT("turn:127.0.0.1:9?transport=udp"), 0);
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0 || Travel->bAnswered; });
	TestEqual(TEXT("connected through the host's server"), F.Game->TraveledTo.Num(), 1);
	TestEqualSensitive(TEXT("on its server"), F.SessionsRelay().IsValid() ? F.SessionsRelay()->GetServer() : FString(), F.RelayServer());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelServerNotListedTest, "Player.APlayerWhoseLoginsLackTheHostsServer_RelayFailed_ServerNotListed")
bool FFlockRelayTravelServerNotListedTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), TEXT("relay.example:3478")));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return Travel->bAnswered; });
	TestEqualSensitive(TEXT("relay_failed"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::RelayFailed));
	TestEqualSensitive(TEXT("relay_server_not_listed"), Travel->Result.Value.RelayFailureReason, FString(FlockRelayFailureReasons::ServerNotListed));
	TestEqual(TEXT("no travel"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelAlwaysDirectHostTest, "Player.AnAlwaysPlayer_OfADirectOnlyHost_RelayFailed_WithoutOpeningARelay")
bool FFlockRelayTravelAlwaysDirectHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, Direct(Text(HostsPublicAddress), Number(7777)));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return Travel->bAnswered; });
	TestEqualSensitive(TEXT("relay_failed"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::RelayFailed));
	TestEqualSensitive(TEXT("the host offers none"), Travel->Result.Value.RelayFailureReason, FString(FlockRelayFailureReasons::NotOfferedByTheHost));
	TestEqual(TEXT("no relay opened"), F.RelayLoginsAsked(), 0);
	TestEqual(TEXT("said why"), F.WarningsContaining(TEXT("Relay is Always")), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelNeverRelayOnlyHostTest, "Player.ANeverPlayer_OfARelayOnlyHost_CouldNotConnect_AndSaysWhy")
bool FFlockRelayTravelNeverRelayOnlyHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), F.RelayServer()));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Never));
	F.Until([&]() { return Travel->bAnswered; });
	TestEqualSensitive(TEXT("could_not_connect"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestEqual(TEXT("said why"), F.WarningsContaining(TEXT("Relay is Never")), 1);
	TestEqual(TEXT("no relay opened"), F.RelayLoginsAsked(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelNeverWhenNeededHostTest, "Player.ANeverPlayer_OfAHostWithDirectAddressesToo_ConnectsDirectly")
bool FFlockRelayTravelNeverWhenNeededHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), F.RelayServer(), /*bWithDirect*/ true,
		TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Travel(Session, Relaying(EFlockRelayUse::Never));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqualSensitive(TEXT("to the host's published address"), F.Game->TraveledTo.Num() > 0 ? F.Game->TraveledTo[0] : FString(), FString(TEXT("198.51.100.4:7777")));
	TestEqual(TEXT("no relay opened"), F.RelayLoginsAsked(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelNoUsableRelayAddressTest, "Player.APlayerOfAHostWithNoUsableRelayAddress_CouldNotConnect")
bool FFlockRelayTravelNoUsableRelayAddressTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedRef<FJsonObject> Broken = MakeShared<FJsonObject>();
	Broken->SetStringField(TEXT("mode"), TEXT("relay"));
	Broken->SetStringField(TEXT("relay_address"), TEXT("not an address"));
	Broken->SetStringField(TEXT("relay_server"), F.RelayServer());
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, Broken);
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return Travel->bAnswered; });
	TestEqualSensitive(TEXT("could_not_connect"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestEqual(TEXT("said why"), F.WarningsContaining(TEXT("no usable relay address")), 1);
	// A relay address with no server named: reserved anywhere, it would share no IP with the host's.
	FRelayTravelFixture G;
	const TSharedRef<FJsonObject> NoServer = MakeShared<FJsonObject>();
	NoServer->SetStringField(TEXT("mode"), TEXT("relay"));
	NoServer->SetStringField(TEXT("relay_address"), TEXT("203.0.113.50:50000"));
	UFlockMultiplayerSession* Unnamed = G.PlayerOf(*this, NoServer);
	const TSharedRef<FTravelAnswer> NoServerTravel = G.Travel(Unnamed, Relaying(EFlockRelayUse::Always));
	G.Until([&]() { return NoServerTravel->bAnswered; });
	TestEqualSensitive(TEXT("could_not_connect with no server named"), NoServerTravel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestEqual(TEXT("no relay reserved"), G.RelayLoginsAsked(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelOnTheHostsNetworkTest, "Player.AWhenNeededPlayer_OnTheHostsNetwork_ConnectsDirectly_WithoutTheRelay")
bool FFlockRelayTravelOnTheHostsNetworkTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), F.RelayServer(), /*bWithDirect*/ true));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqualSensitive(TEXT("to the host's LAN address"), F.Game->TraveledTo.Num() > 0 ? F.Game->TraveledTo[0] : FString(), FString(TEXT("192.168.1.20:7777")));
	F.Game->HostAnswers();
	F.Advance(5.0);
	F.Game->Arrives(true);
	F.RelayFrame();
	TestTrue(TEXT("connected"), Travel->Succeeded() && Travel->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive));
	TestFalse(TEXT("directly"), Travel->Result.Value.bThroughRelay);
	TestEqual(TEXT("no relay opened"), F.RelayLoginsAsked(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelNotAnsweredTest, "Player.AWhenNeededPlayer_WhoseHostsNetworkAddressDoesNotAnswer_ConnectsThroughTheRelay")
bool FFlockRelayTravelNotAnsweredTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Advance(FFlockTravels::TryOnTheHostsNetworkSeconds - 0.1);
	TestEqual(TEXT("still trying before its time"), F.Game->StoppedConnecting, 0);
	F.Advance(0.2);
	TestEqual(TEXT("the try given up once its time ran out unanswered"), F.Game->StoppedConnecting, 1);
	F.Until([&]() { return F.Game->TraveledTo.Num() > 1; });
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	TestTrue(TEXT("then through the relay"), Bridge.IsValid() && F.Game->TraveledTo.Num() == 2
		&& F.Game->TraveledTo[1] == FString::Printf(TEXT("127.0.0.1:%d"), Bridge->GetLocalPort()));
	TestEqual(TEXT("said at Info"), F.InfosContaining(TEXT("did not answer, so this player connects through Flock's relay")), 1);
	TestEqual(TEXT("a token for each try"), F.Server->Count(ERoute::JoinToken), 2);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelEngineGivesUpTest, "Player.AWhenNeededPlayer_WhoseEngineGivesUpOnTheHostsNetwork_GoesThroughTheRelayAtOnce")
bool FFlockRelayTravelEngineGivesUpTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true));
	F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Fails(TEXT("ConnectionTimeout: UNetConnection::Tick: Connection TIMED OUT."));
	F.Game->GoesBackToItsDefaultLevel();
	F.Until([&]() { return F.Game->TraveledTo.Num() > 1; });
	TestEqual(TEXT("through the relay before the try's time"), F.Game->TraveledTo.Num(), 2);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRefusedOnTheNetworkTest, "Player.AWhenNeededPlayer_RefusedByTheHostOnItsNetwork_IsNotTriedThroughTheRelay")
bool FFlockRelayTravelRefusedOnTheNetworkTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), F.RelayServer(), /*bWithDirect*/ true));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->HostAnswers();
	F.RelayFrame();
	F.Game->Fails(FString(TEXT("PendingConnectionFailure: ")) + FlockHostRefusals::WordsFor(FlockTravelToSessionRefusedReasons::JoinTokenInvalid));
	F.Game->GoesBackToItsDefaultLevel();
	F.Until([&]() { return Travel->bAnswered; });
	TestEqualSensitive(TEXT("refused"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Refused));
	TestEqual(TEXT("no relay"), F.RelayLoginsAsked(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSlowCheckTest, "Player.AWhenNeededPlayer_WhoseHostChecksSlowly_StaysOnTheHostsNetwork")
bool FFlockRelayTravelSlowCheckTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), F.RelayServer(), /*bWithDirect*/ true));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	// The host's engine answered at once; its check of the player takes long.
	F.Game->HostAnswers();
	F.RelayFrame();
	F.Game->State.bHostAnswered = false;
	F.Advance(FFlockTravels::TryOnTheHostsNetworkSeconds * 3);
	TestEqual(TEXT("never given up for the relay"), F.Game->StoppedConnecting, 0);
	F.Game->Arrives(true);
	F.RelayFrame();
	TestTrue(TEXT("connected on the host's network"), Travel->Succeeded() && !Travel->Result.Value.bThroughRelay);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelGameStopsTest, "Player.AWhenNeededPlayer_WhoseGameStopsOnTheHostsNetwork_DoesNotGoThroughTheRelay")
bool FFlockRelayTravelGameStopsTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), F.RelayServer(), /*bWithDirect*/ true));
	FFlockRequestHandle Handle;
	const TSharedRef<FTravelAnswer> Cancelled = F.Travel(Session, FFlockTravelToSessionOptions(), &Handle);
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	Handle.Cancel();
	F.Advance(FFlockTravels::TryOnTheHostsNetworkSeconds + 1.0);
	TestTrue(TEXT("the cancel is the game's"), Cancelled->FailedAs(EFlockErrorType::Cancelled));
	// The game leaves for a level of its own while the try runs: no failure told, so it is the game's doing too.
	const TSharedRef<FTravelAnswer> Left = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 1; });
	F.Game->GoesBackToItsDefaultLevel();
	F.Until([&]() { return Left->bAnswered; });
	TestEqualSensitive(TEXT("could_not_connect"), Left->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestEqual(TEXT("never the relay"), F.RelayLoginsAsked(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelEndsDuringTryTest, "Player.AWhenNeededPlayer_WhoseSessionEndsDuringTheTry_IsToldItEnded_WithoutTheRelay")
bool FFlockRelayTravelEndsDuringTryTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(FFlockRelayPeer(0xCB007132, 50000), F.RelayServer(), /*bWithDirect*/ true));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	// The heartbeat brings the end in the frames the try runs out.
	F.Server->Ends(F.Served);
	F.Advance(20.0);
	F.Until([&]() { return Travel->bAnswered; });
	TestEqualSensitive(TEXT("session_ended"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::SessionEnded));
	TestEqual(TEXT("no relay"), F.RelayLoginsAsked(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelWaitsForTheTryTest, "Player.AWhenNeededPlayer_GoesThroughTheRelay_OnlyOnceTheTryHasStopped")
bool FFlockRelayTravelWaitsForTheTryTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true));
	F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Advance(FFlockTravels::TryOnTheHostsNetworkSeconds + 0.1);
	// The engine is still dropping the connection it gave up.
	F.Game->State.bChangingLevel = true;
	F.Until([]() { return false; }, 0.3);
	TestEqual(TEXT("nothing travels while the engine settles"), F.Game->TraveledTo.Num(), 1);
	F.Game->State.bChangingLevel = false;
	F.Until([&]() { return F.Game->TraveledTo.Num() > 1; });
	TestEqual(TEXT("then through the relay"), F.Game->TraveledTo.Num(), 2);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelLanOnlyHostTest, "Player.AWhenNeededPlayer_OfAHostThatKnewOnlyItsLanAddress_GoesThroughTheRelay")
bool FFlockRelayTravelLanOnlyHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true,
		TEXT("192.168.1.20"), FString()));
	F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestTrue(TEXT("through the relay at once"), F.Game->TraveledTo.Num() == 1 && F.Game->TraveledTo[0].StartsWith(TEXT("127.0.0.1:")));
	// A device that does not know its own public address either: two unknown addresses are no shared network.
	FRelayTravelFixture G;
	G.Stun->bSilent = true;
	const TSharedPtr<FFlockRelayConnection> Hosting = G.Device(*this, PlayerA);
	if (!Hosting.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Unknown = G.PlayerOf(*this, G.RelayConnectionOf(Hosting->GetAddress(), G.RelayServer(), /*bWithDirect*/ true,
		TEXT("192.168.1.20"), FString()));
	G.Travel(Unknown, FFlockTravelToSessionOptions());
	G.Until([&]() { return G.Game->TraveledTo.Num() > 0; });
	TestTrue(TEXT("through the relay too, with no public address known on either side"),
		G.Game->TraveledTo.Num() == 1 && G.Game->TraveledTo[0].StartsWith(TEXT("127.0.0.1:")));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelOffTheNetworkTest, "Player.AWhenNeededPlayer_OffTheHostsNetwork_ConnectsThroughTheRelay")
bool FFlockRelayTravelOffTheNetworkTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true,
		TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestTrue(TEXT("through the relay, not to the host's own address"), F.Game->TraveledTo.Num() == 1 && F.Game->TraveledTo[0].StartsWith(TEXT("127.0.0.1:")));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRefusedClosesTest, "Player.ARelayedPlayerRefused_ClosesItsBridge_KeepsTheSessionsRelay_AndConnectsWhenItTravelsAgain")
bool FFlockRelayTravelRefusedClosesTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	const TSharedRef<FTravelAnswer> Again = MakeShared<FTravelAnswer>();
	FFlockRelayBridge::FPtr Refused;
	const TSharedRef<FTravelAnswer> Travel = MakeShared<FTravelAnswer>();
	const TWeakObjectPtr<UFlockMultiplayerSession> WeakSession = Session;
	Session->TravelToSession(nullptr, Relaying(EFlockRelayUse::Always), [Travel, Again, WeakSession](TFlockResult<FFlockTravelToSessionResult> Result)
	{
		Travel->bAnswered = true;
		Travel->Result = Result;
		// Travelling again from the refusal's own answer, as a game's retry button does.
		if (Result.bSuccess && WeakSession.IsValid())
		{
			WeakSession->TravelToSession(nullptr, Relaying(EFlockRelayUse::Always), Into(Again));
		}
	});
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	Refused = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	F.Game->Fails(FString(TEXT("PendingConnectionFailure: ")) + FlockHostRefusals::WordsFor(FlockTravelToSessionRefusedReasons::FlockUnreachable));
	F.Game->GoesBackToItsDefaultLevel();
	F.Until([&]() { return Travel->bAnswered; });
	TestEqualSensitive(TEXT("refused"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Refused));
	TestTrue(TEXT("the refused bridge closed"), Refused.IsValid() && Refused->IsClosed());
	TestTrue(TEXT("the session's relay stays"), F.SessionsRelay().IsValid() && F.SessionsRelay()->IsOpen());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 1; });
	TestEqual(TEXT("the second travel went to a new bridge"), F.Game->TraveledTo.Num(), 2);
	const FFlockRelayBridge::FPtr Next = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	TestTrue(TEXT("over the same relay"), Next.IsValid() && Next != Refused && !Next->IsClosed() && Next->GetRelay() == F.SessionsRelay().ToSharedRef());
	TestEqual(TEXT("the seat said its address once"), F.Server->Count(ERoute::PublishSeat), 1);
	F.Game->Arrives(true);
	F.RelayFrame();
	TestTrue(TEXT("connected"), Again->Succeeded() && Again->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelLeftTheHostTest, "Player.APlayersBridge_ClosesWhenItsGameLeavesTheHost")
bool FFlockRelayTravelLeftTheHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.RelayFrame();
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	if (!TestTrue(TEXT("precondition: connected through a bridge"), Travel->Succeeded() && Bridge.IsValid()))
	{
		return false;
	}
	F.RelayFrame();
	TestFalse(TEXT("kept while connected"), Bridge->IsClosed());
	F.Game->GoesBackToItsDefaultLevel();
	F.RelayFrame();
	TestTrue(TEXT("closed once the game left its host"), Bridge->IsClosed());
	TestFalse(TEXT("with its host no more"), F.Sessions().GetTravels().IsWithTheHostForTesting());
	TestTrue(TEXT("the session's relay stays"), F.SessionsRelay().IsValid() && F.SessionsRelay()->IsOpen());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReconnectThroughBridgeTest, "Player.APlayersBridge_StaysWhileItsGameConnectsToTheHostAgain_WithTheTokenOnHand")
bool FFlockRelayTravelReconnectThroughBridgeTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.RelayFrame();
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	if (!Bridge.IsValid())
	{
		return false;
	}
	const FString Where = FString::Printf(TEXT("127.0.0.1:%d"), Bridge->GetLocalPort());
	const FString First = F.Game->TokensTraveledWith[0];
	// Past the first token's 40 s: a new one is on hand.
	F.Advance(41.0);
	F.RelayFrame();
	const int32 Asked = F.Server->Count(ERoute::JoinToken);
	// The host's level change without seamless travel sends this game back to it with the first travel's options (measured).
	F.Game->ConnectsAgain(FString::Printf(TEXT("%s/Game/Maps/Next?FlockJoinToken=%s?game=/Script/Game.Mode?Name=Ann"), *Where, *First));
	F.RelayFrame();
	TestEqual(TEXT("the token on hand put on the engine's connection"), F.Game->TokensPut, 1);
	TestTrue(TEXT("the bridge told its game connects again, so the host takes the new connection from a place of its own"),
		Bridge->IsToldTheGameConnectsAgainForTesting());
	TestEqual(TEXT("at once: nothing asked for it"), F.Server->Count(ERoute::JoinToken), Asked);
	TestFalse(TEXT("which logs in with it"), F.Game->LoginToken() == First || F.Game->LoginToken().IsEmpty());
	TestTrue(TEXT("its other options kept"), F.Game->ConnectingUrl.Contains(TEXT("?game=/Script/Game.Mode")) && F.Game->ConnectingUrl.Contains(TEXT("?Name=Ann")));
	TestEqual(TEXT("one token in the URL"), F.Game->ConnectingUrl.Find(TEXT("FlockJoinToken")),
		F.Game->ConnectingUrl.Find(TEXT("FlockJoinToken"), ESearchCase::IgnoreCase, ESearchDir::FromEnd));
	TestEqual(TEXT("the engine's own connection: no travel of Flock's"), F.Game->Urls.Num(), 1);
	F.RelayFrame();
	TestFalse(TEXT("the bridge stays while the game reconnects"), Bridge->IsClosed());
	F.Game->Arrives(true);
	F.RelayFrame();
	TestFalse(TEXT("and once it is back"), Bridge->IsClosed());
	return true;
}

// ---- Travel To Session again through the relay ----

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRestartSameBridgeTest, "Restart.APlayerInItsHostsGameThroughTheRelay_TravelingAgain_IsConnectedAtOnce_ItsBridgeKept")
bool FFlockRelayTravelRestartSameBridgeTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.RelayFrame();
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	if (!TestTrue(TEXT("precondition: connected through a bridge"), Bridge.IsValid()))
	{
		return false;
	}
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	TestTrue(TEXT("connected at once"), Again->Succeeded() && Again->Result.Value.bThroughRelay
		&& Again->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive));
	TestEqual(TEXT("traveling nowhere"), F.Game->TraveledTo.Num(), 1);
	TestTrue(TEXT("the same bridge, still open"), F.Sessions().GetTravels().GetPlayersBridgeForTesting() == Bridge && !Bridge->IsClosed());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRestartLostPlayerTest, "Restart.APlayerWhoseRelayStopped_TravelsAgain_FromTheHostsLevel_ThroughANewRelayAddress")
bool FFlockRelayTravelRestartLostPlayerTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.RelayFrame();
	const TSharedPtr<FFlockRelayConnection> Lost = F.SessionsRelay();
	if (!TestTrue(TEXT("precondition: connected through the relay"), Lost.IsValid() && F.Sessions().GetTravels().IsWithTheHostForTesting()))
	{
		return false;
	}
	// The player's relay stops: the SDK lets its host go at once, though the engine is still in the host's level.
	Lost->Close();
	F.RelayFrame();
	TestFalse(TEXT("precondition: no longer with its host"), F.Sessions().GetTravels().IsWithTheHostForTesting());
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 1 || Again->bAnswered; });
	if (!TestEqual(TEXT("traveled again, from the host's level"), F.Game->TraveledTo.Num(), 2))
	{
		AddInfo(Again->bAnswered ? Again->Result.Error.Message : FString(TEXT("not answered")));
		return false;
	}
	TestTrue(TEXT("through a new relay address"), F.SessionsRelay().IsValid() && F.SessionsRelay() != Lost && !F.SessionsRelay()->HasStopped());
	F.Game->Arrives(true);
	F.RelayFrame();
	TestEqualSensitive(TEXT("connected"), Again->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Connected));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelHostMovedTest, "Restart.APlayerWhoseHostMovedToAnotherRelayAddress_IsToldItsRelayLost_Once")
bool FFlockRelayTravelHostMovedTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	const TSharedPtr<FFlockRelayConnection> Moved = F.Device(*this, PlayerA);
	if (!Host.IsValid() || !Moved.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	UFlockSessionTestListener* Listener = NewObject<UFlockSessionTestListener>();
	const TStrongObjectPtr<UFlockSessionTestListener> KeepListener(Listener);
	Listener->Listen(Session);
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.RelayFrame();
	// The host publishes again on the address it had: nothing moved.
	F.Server->Publishes(F.Served, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Provider->GetSession(Session->GetId(), [](TFlockResult<UFlockMultiplayerSession*>) {});
	F.RelayFrame();
	TestEqual(TEXT("control: the same address tells nothing"), Listener->RelayLost.Num(), 0);
	// It lost its relay and published another address: this player's bridge reaches the old one.
	F.Server->Publishes(F.Served, F.RelayConnectionOf(Moved->GetAddress(), F.RelayServer()));
	F.Provider->GetSession(Session->GetId(), [](TFlockResult<UFlockMultiplayerSession*>) {});
	F.RelayFrame();
	F.RelayFrame();
	if (TestEqual(TEXT("told its relay lost"), Listener->RelayLost.Num(), 1))
	{
		TestEqualSensitive(TEXT("as lost"), Listener->RelayLost[0], FString(FlockRelayFailureReasons::Lost));
	}
	TestEqual(TEXT("and said why"), F.WarningsContaining(TEXT("host's address on Flock's relay changed")), 1);
	F.RelayFrame();
	TestEqual(TEXT("once"), Listener->RelayLost.Num(), 1);
	// Travel To Session again connects to the new address.
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 1; });
	TestEqual(TEXT("travels again, to the host's new address"), F.Game->TraveledTo.Num(), 2);
	// Hosting handed to another player, who publishes a relay address of its own: On Host Changed tells that, not On Relay Lost
	// (measured on Cloudflare's relay, 2026-10-19: a player that stayed was told its relay lost as the new host published).
	FRelayTravelFixture G;
	const TSharedPtr<FFlockRelayConnection> OldHost = G.Device(*this, PlayerA);
	const TSharedPtr<FFlockRelayConnection> NewHost = G.Device(*this, PlayerC);
	if (!OldHost.IsValid() || !NewHost.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Changing = G.PlayerOf(*this, G.RelayConnectionOf(OldHost->GetAddress(), G.RelayServer()));
	UFlockSessionTestListener* Hears = NewObject<UFlockSessionTestListener>();
	const TStrongObjectPtr<UFlockSessionTestListener> KeepHears(Hears);
	Hears->Listen(Changing);
	G.Travel(Changing, Relaying(EFlockRelayUse::Always));
	G.Until([&]() { return G.Game->TraveledTo.Num() > 0; });
	G.Game->Arrives(true);
	G.RelayFrame();
	G.Server->Joins(G.Served, PlayerC);
	G.Server->MakesHost(G.Served, PlayerC);
	G.Server->Publishes(G.Served, G.RelayConnectionOf(NewHost->GetAddress(), G.RelayServer()));
	G.Provider->GetSession(Changing->GetId(), [](TFlockResult<UFlockMultiplayerSession*>) {});
	G.RelayFrame();
	G.RelayFrame();
	TestEqual(TEXT("a new host: told as a host change"), Hears->HostChanged, 1);
	TestEqual(TEXT("not as a relay lost"), Hears->RelayLost.Num(), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRestartLostHostTest, "Restart.AHostWhoseRelayWasLost_TravelsAgain_AndPublishesANewRelayAddress_KeepingItsLevel")
bool FFlockRelayTravelRestartLostHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	FFlockRelayTiming Timing = FlockRelayTestHelpers::Quick();
	Timing.LongestBetweenReservationRenewalsSeconds = 0.5;
	F.Sessions().SetRelayTimingForTesting(Timing);
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session);
	if (Session == nullptr || !TestTrue(TEXT("precondition: hosting through the relay"), Hosting->Succeeded() && Hosting->Result.Value.bThroughRelay))
	{
		return false;
	}
	UFlockSessionTestListener* Listener = NewObject<UFlockSessionTestListener>();
	const TStrongObjectPtr<UFlockSessionTestListener> KeepListener(Listener);
	Listener->Listen(Session);
	const FString Before = F.Field(F.LastPublished(), TEXT("relay_address"));
	F.Relay->ForgetEveryReservation();
	F.Until([&]() { return Listener->RelayLost.Num() > 0; }, 3.0);
	if (!TestEqual(TEXT("precondition: told its relay was lost"), Listener->RelayLost.Num(), 1))
	{
		return false;
	}
	TestFalse(TEXT("its players' bridge closed with the relay"), F.HostsBridge().IsValid());
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return Again->bAnswered; });
	TestTrue(TEXT("hosts again through the relay"), Again->Succeeded() && Again->Result.Value.bThroughRelay
		&& Again->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::StartedAsHost, ESearchCase::CaseSensitive));
	const FString After = F.Field(F.LastPublished(), TEXT("relay_address"));
	TestTrue(TEXT("on a new relay address, published"), !After.IsEmpty() && !After.Equals(Before, ESearchCase::CaseSensitive));
	TestTrue(TEXT("with a bridge for its players again"), F.HostsBridge().IsValid());
	TestEqual(TEXT("its level kept"), F.Game->Opened.Num(), 1);
	TestEqual(TEXT("still listening"), F.Game->StoppedListening, 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRestartKeepRelayTest, "Restart.AHostThroughTheRelay_TravelingAgainWithItsRelayWorking_KeepsItsBridgeAndAddress")
bool FFlockRelayTravelRestartKeepRelayTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Hosting = F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session);
	if (Session == nullptr || !TestTrue(TEXT("precondition: hosting through the relay"), Hosting->Succeeded() && Hosting->Result.Value.bThroughRelay))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.HostsBridge();
	const FString Before = F.Field(F.LastPublished(), TEXT("relay_address"));
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return Again->bAnswered; });
	TestTrue(TEXT("hosts again through the relay"), Again->Succeeded() && Again->Result.Value.bThroughRelay
		&& Again->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::StartedAsHost, ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("on the address it had"), F.Field(F.LastPublished(), TEXT("relay_address")), Before);
	TestTrue(TEXT("its players' bridge kept"), Bridge.IsValid() && F.HostsBridge() == Bridge && !Bridge->IsClosed());
	TestEqual(TEXT("no new relay login"), F.RelayLoginsAsked(), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRestartNewHostTest, "Restart.APlayerThroughTheRelayThatBecameTheHost_HostsOnTheRelayAddressItHad")
bool FFlockRelayTravelRestartNewHostTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.RelayFrame();
	const FFlockRelayBridge::FPtr PlayersBridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	if (!TestTrue(TEXT("precondition: connected through the relay"), PlayersBridge.IsValid() && Relay.IsValid()))
	{
		return false;
	}
	F.Server->Leaves(F.Served, PlayerA);
	F.Provider->GetSession(Session->GetId(), [](TFlockResult<UFlockMultiplayerSession*>) {});
	if (!TestTrue(TEXT("precondition: B hosts"), Session->IsHost()))
	{
		return false;
	}
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	TestTrue(TEXT("its player's bridge made way for the host's"), PlayersBridge->IsClosed());
	F.Game->Opens(F.Engine.Port);
	F.Until([&]() { return Again->bAnswered; });
	TestTrue(TEXT("hosts through the relay"), Again->Succeeded() && Again->Result.Value.bThroughRelay
		&& Again->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::StartedAsHost, ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("on the relay address it had"), F.Field(F.LastPublished(), TEXT("relay_address")), Relay->GetAddress().ToString());
	TestEqual(TEXT("no new relay login"), F.RelayLoginsAsked(), 1);
	TestTrue(TEXT("a bridge for its players"), F.HostsBridge().IsValid());
	return true;
}

// ---- a join token kept on hand for every connection made to the host again (direct connections too) ----

namespace FlockRelayTravelReconnectHelpers
{
	const TCHAR* const Back = TEXT("198.51.100.4:7777/Game/Maps/Next?FlockJoinToken=first-token?Name=Ann");

	/** A player connected directly to its host, its first travel's join token on hand; its session, or null when it did not connect. */
	UFlockMultiplayerSession* ConnectedDirectly(FAutomationTestBase& Test, FRelayTravelFixture& F)
	{
		UFlockMultiplayerSession* Session = F.PlayerOf(Test, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
		const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, Relaying(EFlockRelayUse::Never));
		F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
		F.Game->Arrives(true);
		F.RelayFrame();
		return Test.TestTrue(TEXT("precondition: connected"), Travel->Succeeded()) ? Session : nullptr;
	}

	/** The token on hand nearly spent (9 s left) and its replacement refused: a connection back must ask for its own. */
	void NoGoodTokenOnHand(FRelayTravelFixture& F)
	{
		F.Server->AnswerNext(ERoute::JoinToken, FFlockFakeSessionServer::Refused(503, TEXT("service.unavailable")));
		F.Advance(51.0);
		F.RelayFrame();
	}
}

using namespace FlockRelayTravelReconnectHelpers;

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelTokenOnHandTest, "Reconnect.AConnectedPlayer_KeepsAFreshJoinTokenOnHand_AskingAboutEvery40s")
bool FFlockRelayTravelTokenOnHandTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	// The first travel's own token is the first on hand: nothing more is asked as the player connects.
	TestEqual(TEXT("only the first travel's token asked"), F.Server->Count(ERoute::JoinToken), 1);
	const int32 Before = F.Server->Count(ERoute::JoinToken);
	F.Advance(39.0);
	F.RelayFrame();
	TestEqual(TEXT("none while the token on hand has 20 s or more"), F.Server->Count(ERoute::JoinToken), Before);
	F.Advance(2.0);
	F.RelayFrame();
	TestEqual(TEXT("one once it has less"), F.Server->Count(ERoute::JoinToken), Before + 1);
	for (int32 Second = 0; Second < 159; ++Second)
	{
		F.Advance(1.0);
	}
	F.RelayFrame();
	TestEqual(TEXT("about every 40 s: at 41, 81, 121 and 161 s"), F.Server->Count(ERoute::JoinToken), Before + 4);
	// A game that left its host keeps no token on hand.
	F.Game->GoesBackToItsDefaultLevel();
	F.RelayFrame();
	const int32 Left = F.Server->Count(ERoute::JoinToken);
	F.Advance(100.0);
	F.RelayFrame();
	TestEqual(TEXT("none once it left its host"), F.Server->Count(ERoute::JoinToken), Left);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelDirectReconnectTest, "Reconnect.AConnectionMadeToTheHostAgain_LogsInWithTheTokenOnHand_AskingNothing")
bool FFlockRelayTravelDirectReconnectTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	F.Advance(41.0);
	F.RelayFrame();
	const int32 Asked = F.Server->Count(ERoute::JoinToken);
	F.Game->ConnectsAgain(Back);
	F.RelayFrame();
	TestEqual(TEXT("nothing asked for the connection"), F.Server->Count(ERoute::JoinToken), Asked);
	TestEqual(TEXT("the token on hand put on it"), F.Game->TokensPut, 1);
	TestNotEqual(TEXT("not the first token"), F.Game->LoginToken(), FString(TEXT("first-token")));
	TestNotEqual(TEXT("the newer token on hand, not the first travel's"), F.Game->LoginToken(), F.Game->TokensTraveledWith[0]);
	TestEqualSensitive(TEXT("in the first one's place, the rest kept"), F.Game->ConnectingUrl,
		FString::Printf(TEXT("198.51.100.4:7777/Game/Maps/Next?FlockJoinToken=%s?Name=Ann"), *F.Game->LoginToken()));
	F.RelayFrame();
	TestEqual(TEXT("once for the connection"), F.Game->TokensPut, 1);
	TestTrue(TEXT("still with its host"), F.Sessions().GetTravels().IsWithTheHostForTesting());
	// Straight after the first travel, its own token is the one on hand, good for most of its 60 s.
	FRelayTravelFixture G;
	if (!ConnectedDirectly(*this, G))
	{
		return false;
	}
	const int32 AskedFirst = G.Server->Count(ERoute::JoinToken);
	G.Game->ConnectsAgain(Back);
	G.RelayFrame();
	TestEqualSensitive(TEXT("the first travel's own token on hand"), G.Game->LoginToken(), G.Game->TokensTraveledWith[0]);
	TestEqual(TEXT("asking nothing"), G.Server->Count(ERoute::JoinToken), AskedFirst);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReconnectAsksTest, "Reconnect.AConnectionBack_WithNoGoodTokenOnHand_AsksForOne_AndLogsInWithIt")
bool FFlockRelayTravelReconnectAsksTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	NoGoodTokenOnHand(F);
	const int32 Asked = F.Server->Count(ERoute::JoinToken);
	F.Game->ConnectsAgain(Back);
	F.RelayFrame();
	TestEqual(TEXT("one asked for the connection, at once"), F.Server->Count(ERoute::JoinToken), Asked + 1);
	TestEqual(TEXT("and put on it"), F.Game->TokensPut, 1);
	TestFalse(TEXT("a new token, not the first"), F.Game->LoginToken().IsEmpty() || F.Game->LoginToken() == TEXT("first-token"));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelAskAgainTest, "Reconnect.ATokenFlockDidNotGive_IsAskedForAgainTenSecondsLater")
bool FFlockRelayTravelAskAgainTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::JoinToken, FFlockFakeSessionServer::Refused(503, TEXT("service.unavailable")));
	F.Advance(41.0);
	F.RelayFrame();
	const int32 Asked = F.Server->Count(ERoute::JoinToken);
	F.RelayFrame();
	F.Advance(5.0);
	F.RelayFrame();
	TestEqual(TEXT("not asked again every frame"), F.Server->Count(ERoute::JoinToken), Asked);
	F.Advance(5.0);
	F.RelayFrame();
	TestEqual(TEXT("asked again 10 s on"), F.Server->Count(ERoute::JoinToken), Asked + 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelTokenAfterTheEndTest, "Reconnect.ATokenOnHandAskedForOnceTheHostEndedTheSession_LearnsTheEnd_LoggingNoError")
bool FFlockRelayTravelTokenAfterTheEndTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = ConnectedDirectly(*this, F);
	if (Session == nullptr)
	{
		return false;
	}
	// The host ends the session; this game has not heard (its heartbeat comes every 20 s) when the token on hand runs low.
	F.Server->Ends(F.Served);
	const int32 Errors = F.Log->Errors.Num();
	F.Advance(41.0);
	F.RelayFrame();
	F.RelayFrame();
	TestTrue(TEXT("the session's end is learned from Flock's answer"), Session->HasEnded());
	TestEqualSensitive(TEXT("as the host's end"), Session->GetEndReason(), FString(TEXT("ended_by_host")));
	TestEqual(TEXT("no error logged for it"), F.Log->Errors.Num(), Errors);
	TestFalse(TEXT("the player is no longer with its host"), F.Sessions().GetTravels().IsWithTheHostForTesting());
	const int32 Asked = F.Server->Count(ERoute::JoinToken);
	F.Advance(100.0);
	F.RelayFrame();
	TestEqual(TEXT("and no token is asked for any more"), F.Server->Count(ERoute::JoinToken), Asked);
	// Control: the same answer to a token the game asked for itself is logged as the error it is.
	FRelayTravelFixture G;
	UFlockMultiplayerSession* Asking = ConnectedDirectly(*this, G);
	if (Asking == nullptr)
	{
		return false;
	}
	G.Server->Ends(G.Served);
	const int32 ErrorsBefore = G.Log->Errors.Num();
	Asking->RequestJoinToken([](TFlockResult<FFlockMultiplayerSessionJoinToken>) {});
	G.RelayFrame();
	TestTrue(TEXT("control: a game's own token request that fails so is logged"), FFlockRecordingLogger::AnyContains(
		TArray<FString>(G.Log->Errors.GetData() + ErrorsBefore, G.Log->Errors.Num() - ErrorsBefore), TEXT("Request join token")));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReconnectTokenRefusedTest, "Reconnect.AConnectionBackWhoseTokenFlockRefuses_GoesWithTheFirstOne_AndSaysWhy")
bool FFlockRelayTravelReconnectTokenRefusedTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	NoGoodTokenOnHand(F);
	F.Server->AnswerNext(ERoute::JoinToken, FFlockFakeSessionServer::Refused(429, TEXT("request.rate_limited")));
	F.Game->ConnectsAgain(Back);
	F.RelayFrame();
	TestEqualSensitive(TEXT("the engine's own connection goes on with the first"), F.Game->LoginToken(), FString(TEXT("first-token")));
	TestEqual(TEXT("said why"), F.WarningsContaining(TEXT("No new join token for the connection back to the host")), 1);
	TestEqual(TEXT("with Flock's reason"), F.Log->Warnings.FilterByPredicate([](const FString& Line)
	{
		return Line.Contains(TEXT("No new join token for the connection back to the host")) && Line.Contains(TEXT("rate"));
	}).Num(), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReconnectLateTokenTest, "Reconnect.ATokenThatComesAfterTheHostAnswered_ChangesNothing_AndIsSaid")
bool FFlockRelayTravelReconnectLateTokenTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	NoGoodTokenOnHand(F);
	const TSharedRef<FFlockFakePartyServer::FHeld> Token = F.Server->HoldNext(ERoute::JoinToken);
	F.Game->ConnectsAgain(Back);
	F.RelayFrame();
	// The host's engine answers before Flock does: the login goes out with the first token.
	F.Game->HostAnswers();
	Token->Release();
	F.RelayFrame();
	TestEqual(TEXT("the late token is put nowhere"), F.Game->TokensPut, 0);
	TestEqualSensitive(TEXT("the login's token stays the first"), F.Game->LoginToken(), FString(TEXT("first-token")));
	TestEqual(TEXT("said"), F.WarningsContaining(TEXT("came after the connection had logged in")), 1);
	TestEqual(TEXT("once"), F.WarningsContaining(TEXT("No new join token")), 1);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReconnectStopsAskingTest, "Reconnect.AConnectionThatLoggedInBeforeItsToken_IsTold_AndTheTokenIsKeptForTheNext")
bool FFlockRelayTravelReconnectStopsAskingTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	NoGoodTokenOnHand(F);
	const TSharedRef<FFlockFakePartyServer::FHeld> Token = F.Server->HoldNext(ERoute::JoinToken);
	F.Game->ConnectsAgain(Back);
	F.RelayFrame();
	const int32 Asked = F.Server->Count(ERoute::JoinToken);
	// The connection is made, with the first token, while Flock has not answered.
	F.Game->Arrives(true);
	F.RelayFrame();
	TestEqual(TEXT("said"), F.WarningsContaining(TEXT("had not answered before the connection logged in")), 1);
	Token->Release();
	F.RelayFrame();
	TestEqual(TEXT("the token that came after is put on no connection"), F.Game->TokensPut, 0);
	TestEqual(TEXT("and said once in all"), F.WarningsContaining(TEXT("No new join token")), 1);
	// It is kept on hand: the next connection back takes it at once.
	F.Game->ConnectsAgain(Back);
	F.RelayFrame();
	TestEqual(TEXT("put on the next connection back"), F.Game->TokensPut, 1);
	TestNotEqual(TEXT("the token that came, not the first travel's"), F.Game->LoginToken(), F.Game->TokensTraveledWith[0]);
	TestEqual(TEXT("with nothing more asked"), F.Server->Count(ERoute::JoinToken), Asked);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReconnectAfterLeavingTest, "Reconnect.AConnectionOfAGameThatLeftItsHost_IsLeftToTheEngine")
bool FFlockRelayTravelReconnectAfterLeavingTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	if (!ConnectedDirectly(*this, F))
	{
		return false;
	}
	F.Game->GoesBackToItsDefaultLevel();
	F.RelayFrame();
	const int32 Asked = F.Server->Count(ERoute::JoinToken);
	F.Game->ConnectsAgain(TEXT("198.51.100.4:7777/Game/Maps/Next?FlockJoinToken=first-token"));
	F.RelayFrame();
	TestEqual(TEXT("no token put"), F.Game->TokensPut, 0);
	TestEqual(TEXT("nor asked"), F.Server->Count(ERoute::JoinToken), Asked);
	FRelayTravelFixture G;
	UFlockMultiplayerSession* Ending = ConnectedDirectly(*this, G);
	if (Ending == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Ending->Leave(Into(Left));
	G.RelayFrame();
	G.Game->ConnectsAgain(TEXT("198.51.100.4:7777/Game/Maps/Next?FlockJoinToken=first-token"));
	G.RelayFrame();
	TestEqual(TEXT("nor once the session ended"), G.Game->TokensPut, 0);
	// A game that leaves its host while a token is asked for: the request ends with it, and nothing is said.
	FRelayTravelFixture H(/*MaxRetries*/ 2);
	if (!ConnectedDirectly(*this, H))
	{
		return false;
	}
	H.Server->AnswerNext(ERoute::JoinToken, FFlockFakeSessionServer::Refused(503, TEXT("service.unavailable")));
	H.Advance(41.0);
	H.RelayFrame();
	const int32 AskedBeforeLeaving = H.Server->Count(ERoute::JoinToken);
	H.Game->GoesBackToItsDefaultLevel();
	H.RelayFrame();
	FlockTestRetries::Pump();
	TestEqual(TEXT("a game that left asks for no token any more"), H.Server->Count(ERoute::JoinToken), AskedBeforeLeaving);
	TestEqual(TEXT("and says nothing of it"), H.WarningsContaining(TEXT("No new join token")), 0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatReadTest, "Seats.ASeatsRelayAddressIsReadOnlyFromARelayConnection")
bool FFlockRelayTravelSeatReadTest::RunTest(const FString& Parameters)
{
	const auto Seat = [](const FString& Player, const FString& Mode, const FString& RelayAddress)
	{
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("player_id"), Player);
		Row->SetStringField(TEXT("status"), TEXT("joined"));
		const TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		if (!Mode.IsEmpty())
		{
			Connection->SetStringField(TEXT("mode"), Mode);
			Connection->SetStringField(TEXT("relay_address"), RelayAddress);
		}
		Row->SetObjectField(TEXT("connection_info"), Connection);
		return MakeShared<FJsonValueObject>(Row);
	};
	const TSharedRef<FJsonObject> Answer = MakeShared<FJsonObject>();
	Answer->SetStringField(TEXT("id"), TEXT("01SESSION"));
	Answer->SetArrayField(TEXT("participants"), {
		Seat(TEXT("relayed"), TEXT("relay"), TEXT("203.0.113.50:50000")),
		Seat(TEXT("direct"), TEXT("direct"), TEXT("203.0.113.50:50001")),
		Seat(TEXT("other-case"), TEXT("Relay"), TEXT("203.0.113.50:50002")),
		Seat(TEXT("not-an-address"), TEXT("relay"), TEXT("somewhere")),
		Seat(TEXT("none"), FString(), FString()) });
	FFlockMultiplayerSessionReading Reading;
	FString Error;
	if (!TestTrue(TEXT("read"), FFlockMultiplayerSessionReading::FromWireObject(Answer, Reading, Error)) || !TestEqual(TEXT("five seats"), Reading.Seats.Num(), 5))
	{
		return false;
	}
	TestEqualSensitive(TEXT("a relay connection's address"), Reading.Seats[0].RelayAddress, FString(TEXT("203.0.113.50:50000")));
	TestTrue(TEXT("not from a direct one"), Reading.Seats[1].RelayAddress.IsEmpty());
	TestTrue(TEXT("nor from a mode in other letters"), Reading.Seats[2].RelayAddress.IsEmpty());
	TestTrue(TEXT("nor anything but an address"), Reading.Seats[3].RelayAddress.IsEmpty());
	TestTrue(TEXT("nor from none"), Reading.Seats[4].RelayAddress.IsEmpty());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatSaysBridgeMessagesTest, "Seats.ASeatSaysItsBridgeReadsTheBridgesMessages_OnlyAsANumberInARelayConnection")
bool FFlockRelayTravelSeatSaysBridgeMessagesTest::RunTest(const FString& Parameters)
{
	const auto Seat = [](const FString& Player, const FString& Mode, const TSharedPtr<FJsonValue>& Version)
	{
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("player_id"), Player);
		Row->SetStringField(TEXT("status"), TEXT("joined"));
		const TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetStringField(TEXT("mode"), Mode);
		Connection->SetStringField(TEXT("relay_address"), TEXT("203.0.113.50:50000"));
		if (Version.IsValid())
		{
			Connection->SetField(TEXT("bridge_messages"), Version);
		}
		Row->SetObjectField(TEXT("connection_info"), Connection);
		return MakeShared<FJsonValueObject>(Row);
	};
	const TSharedRef<FJsonObject> Answer = MakeShared<FJsonObject>();
	Answer->SetStringField(TEXT("id"), TEXT("01SESSION"));
	Answer->SetArrayField(TEXT("participants"), {
		Seat(TEXT("reads"), TEXT("relay"), MakeShared<FJsonValueNumber>(1)),
		Seat(TEXT("before-this-version"), TEXT("relay"), nullptr),
		Seat(TEXT("as-text"), TEXT("relay"), MakeShared<FJsonValueString>(TEXT("1"))),
		Seat(TEXT("direct"), TEXT("direct"), MakeShared<FJsonValueNumber>(1)) });
	FFlockMultiplayerSessionReading Reading;
	FString Error;
	if (!TestTrue(TEXT("read"), FFlockMultiplayerSessionReading::FromWireObject(Answer, Reading, Error)) || !TestEqual(TEXT("four seats"), Reading.Seats.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("a relay connection saying version 1"), Reading.Seats[0].bReadsBridgeMessages);
	TestFalse(TEXT("one saying nothing: a bridge that hands them to its engine"), Reading.Seats[1].bReadsBridgeMessages);
	TestFalse(TEXT("one saying it as text"), Reading.Seats[2].bReadsBridgeMessages);
	TestFalse(TEXT("a direct connection"), Reading.Seats[3].bReadsBridgeMessages);
	return true;
}

// ---- direct paths for relayed players ----

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelWhenNeededDirectPathsTest, "DirectPaths.AWhenNeededHost_SaysItsBridgeReadsTheBridgesMessages_AndOffersDirectPaths")
bool FFlockRelayTravelWhenNeededDirectPathsTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, FFlockTravelToSessionOptions(), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Published = F.LastPublished();
	double Version = 0.0;
	TestTrue(TEXT("its relay connection says its bridge reads version 1 of the bridges' messages"), Published.IsValid()
		&& Published->TryGetNumberField(TEXT("bridge_messages"), Version) && Version == 1.0);
	TestTrue(TEXT("its bridge offers its relayed players direct paths"), F.HostsBridge().IsValid() && F.HostsBridge()->GoesDirectForTesting());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelAlwaysNoDirectPathsTest, "DirectPaths.AnAlwaysHost_SaysItsBridgeReadsTheBridgesMessages_AndOffersNoDirectPath")
bool FFlockRelayTravelAlwaysNoDirectPathsTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Published = F.LastPublished();
	double Version = 0.0;
	TestTrue(TEXT("its bridge reads them (a player's word of a new connection)"), Published.IsValid() && Published->TryGetNumberField(TEXT("bridge_messages"), Version) && Version == 1.0);
	TestTrue(TEXT("but offers no direct path: every address stays hidden"), F.HostsBridge().IsValid() && !F.HostsBridge()->GoesDirectForTesting());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelKeptBridgeFollowsTest, "DirectPaths.AHostTravelingAgainWithAnotherRelayChoice_KeepsItsBridge_AndFollowsTheNewChoice")
bool FFlockRelayTravelKeptBridgeFollowsTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, FFlockTravelToSessionOptions(), Session)->Succeeded()) || Session == nullptr)
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.HostsBridge();
	const TSharedRef<FTravelAnswer> Always = F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return Always->bAnswered; });
	TestTrue(TEXT("the same bridge"), Bridge.IsValid() && F.HostsBridge() == Bridge);
	TestFalse(TEXT("no direct path offered once the host chose Always"), Bridge->GoesDirectForTesting());
	const TSharedRef<FTravelAnswer> WhenNeeded = F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return WhenNeeded->bAnswered; });
	TestTrue(TEXT("offered again with When Needed"), F.HostsBridge() == Bridge && Bridge->GoesDirectForTesting());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelReadingTellsBridgeTest, "DirectPaths.TheHostsReadingsOfTheSession_TellItsBridgeWhichPlayersReadTheBridgesMessages")
bool FFlockRelayTravelReadingTellsBridgeTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	F.Sessions().SetRelayedPlayersReadSecondsForTesting(2.0);
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, FFlockTravelToSessionOptions(), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	if (!Player.IsValid())
	{
		return false;
	}
	F.Server->Joins(F.Served, PlayerB);
	// Heard through the relay, so the host reads the session often.
	F.ReachesTheHost(Player, TEXT("handshake"));
	F.Server->SeatSaysRelayAddress(F.Served, PlayerB, Player->GetAddress().ToString());
	F.Advance(2.0);
	F.Until([&]() { return Relay->IsOpenTo(Player->GetAddress()); }, 3.0);
	TestFalse(TEXT("a seat that does not say so: its bridge would hand them to its engine"), F.HostsBridge()->HearsThatItReadsBridgeMessagesForTesting(Player->GetAddress()));
	F.Server->SeatSaysRelayAddress(F.Served, PlayerB, Player->GetAddress().ToString(), /*bReadsBridgeMessages*/ true);
	F.Advance(2.0);
	TestTrue(TEXT("one that says so, from the next reading"), F.Until([&]() { return F.HostsBridge()->HearsThatItReadsBridgeMessagesForTesting(Player->GetAddress()); }, 3.0));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelNewerBeforeSweepTest, "DirectPaths.ARelayedPlayersNewerConnection_LetInBeforeItsOlderIsSeenGone_KeepsItsChannel")
bool FFlockRelayTravelNewerBeforeSweepTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	const int32 Port = Player.IsValid() ? F.ReachesTheHost(Player, TEXT("handshake")) : 0;
	const FJoining Old = F.JoinsThroughTheRelay(F.SeatedWithToken(PlayerB), Port);
	if (!TestTrue(TEXT("precondition: let in"), Old.Answer->LetIn()))
	{
		return false;
	}
	// The player's game connects again through the same relay address; the engine drops its older connection, and the newer one is
	// let in before a frame has seen the older one gone.
	const TSharedRef<FFlockFakePartyServer::FHeld> Verifying = F.Server->HoldNext(ERoute::Verify);
	const FJoining Next = F.JoinsThroughTheRelay(F.Server->MintsJoinToken(F.Served, PlayerB), Port);
	F.Game->Leaves(Old.Connection);
	Verifying->Release();
	if (!TestTrue(TEXT("precondition: the newer let in"), Next.Answer->LetIn()))
	{
		return false;
	}
	F.RelayFrame();
	TestEqual(TEXT("the newer keeps the place"), F.HostsBridge()->LoopbackPortOf(Player->GetAddress()), Port);
	TestTrue(TEXT("and its channel: the older one's going lets go of nothing the newer holds"), Relay->KeepsChannelToForTesting(Player->GetAddress()));
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelMovedPlaceKeptTest, "DirectPaths.APlayerWhosePlaceMovedToANewLoopbackPort_KeepsItWhenItsOlderConnectionGoes")
bool FFlockRelayTravelMovedPlaceKeptTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	if (!TestTrue(TEXT("hosting"), F.HostingWith(*this, Relaying(EFlockRelayUse::Always), Session)->Succeeded()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = F.SessionsRelay();
	const TSharedPtr<FFlockRelayConnection> Player = F.Device(*this, PlayerB, Relay->GetAddress());
	if (!Player.IsValid())
	{
		return false;
	}
	// The player's bridge says its game's first connection starts, then a second one (its game connected again).
	const TArray<uint8> First = FFlockBridgeMessages::WriteNewConnection(1);
	Player->Send(Relay->GetAddress(), First.GetData(), First.Num());
	const int32 Port = F.ReachesTheHost(Player, TEXT("handshake"));
	const FJoining Old = F.JoinsThroughTheRelay(F.SeatedWithToken(PlayerB), Port);
	if (!TestTrue(TEXT("precondition: let in"), Old.Answer->LetIn()))
	{
		return false;
	}
	const TArray<uint8> Second = FFlockBridgeMessages::WriteNewConnection(2);
	Player->Send(Relay->GetAddress(), Second.GetData(), Second.Num());
	F.Until([&]() { return F.HostsBridge()->LoopbackPortOf(Player->GetAddress()) != Port; }, 3.0);
	const int32 Moved = F.HostsBridge()->LoopbackPortOf(Player->GetAddress());
	if (!TestTrue(TEXT("precondition: the newer connection's place, on a port of its own"), Moved > 0 && Moved != Port))
	{
		return false;
	}
	F.Game->Leaves(Old.Connection);
	F.RelayFrame();
	TestEqual(TEXT("the older connection's going leaves the newer's place"), F.HostsBridge()->LoopbackPortOf(Player->GetAddress()), Moved);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelPlayerGoesDirectTest, "DirectPaths.AWhenNeededPlayer_OfAHostOfferingDirectPaths_GetsABridgeThatGoesDirect_AndSaysSoForItsSeat")
bool FFlockRelayTravelPlayerGoesDirectTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	const TSharedRef<FJsonObject> Connection = F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true, TEXT("192.168.1.20"), TEXT("198.51.100.4"));
	Connection->SetNumberField(TEXT("bridge_messages"), 1);
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, Connection);
	F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	TestTrue(TEXT("its bridge has a link to the host"), Bridge.IsValid() && Bridge->GetDirectLinkForTesting(Host->GetAddress()).IsValid());
	const TArray<FFlockHttpRequest> Seat = F.Server->RequestsTo(ERoute::PublishSeat);
	const TSharedPtr<FJsonObject> Body = Seat.Num() == 1 ? BodyOf(Seat[0]) : nullptr;
	const TSharedPtr<FJsonObject>* Published = nullptr;
	double Version = 0.0;
	TestTrue(TEXT("its seat says its bridge reads version 1 of the bridges' messages"), Body.IsValid() && Body->TryGetObjectField(TEXT("connection_info"), Published)
		&& Published != nullptr && (*Published)->TryGetNumberField(TEXT("bridge_messages"), Version) && Version == 1.0);
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelOlderHostNoLinkTest, "DirectPaths.APlayerOfAHostThatDoesNotSayItReadsTheBridgesMessages_GetsNoLink")
bool FFlockRelayTravelOlderHostNoLinkTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	// A host on a version whose bridge hands every packet to its engine, which a bridge message would make close the connection.
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true,
		TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	TestTrue(TEXT("through a bridge"), Bridge.IsValid());
	TestFalse(TEXT("with no link"), Bridge.IsValid() && Bridge->GetDirectLinkForTesting(Host->GetAddress()).IsValid());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelAlwaysPlayerNoLinkTest, "DirectPaths.AnAlwaysPlayer_GetsNoLink_ThoughItsHostOffersDirectPaths")
bool FFlockRelayTravelAlwaysPlayerNoLinkTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	const TSharedRef<FJsonObject> Connection = F.RelayConnectionOf(Host->GetAddress(), F.RelayServer(), /*bWithDirect*/ true, TEXT("192.168.1.20"), TEXT("198.51.100.4"));
	Connection->SetNumberField(TEXT("bridge_messages"), 1);
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, Connection);
	F.Travel(Session, Relaying(EFlockRelayUse::Always));
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	TestTrue(TEXT("through a bridge"), Bridge.IsValid());
	TestFalse(TEXT("with no link: Always keeps this device's addresses from the host"), Bridge.IsValid() && Bridge->GetDirectLinkForTesting(Host->GetAddress()).IsValid());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelRelayOnlyHostNoLinkTest, "DirectPaths.AWhenNeededPlayer_OfAHostOfferingNoDirectAddress_GetsNoLink")
bool FFlockRelayTravelRelayOnlyHostNoLinkTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	// An Always host: its relay address alone, so it offers no direct path.
	const TSharedRef<FJsonObject> Connection = F.RelayConnectionOf(Host->GetAddress(), F.RelayServer());
	Connection->SetNumberField(TEXT("bridge_messages"), 1);
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, Connection);
	F.Travel(Session, FFlockTravelToSessionOptions());
	F.Until([&]() { return F.Game->TraveledTo.Num() > 0; });
	const FFlockRelayBridge::FPtr Bridge = F.Sessions().GetTravels().GetPlayersBridgeForTesting();
	TestTrue(TEXT("through a bridge"), Bridge.IsValid());
	TestFalse(TEXT("with no link"), Bridge.IsValid() && Bridge->GetDirectLinkForTesting(Host->GetAddress()).IsValid());
	return true;
}

FLOCK_RELAY_TRAVEL_TEST(FFlockRelayTravelSeatSaysItAgainTest, "DirectPaths.APlayerWhoseSeatNamesItsRelayAddressButNotThatItReadsTheMessages_PublishesAgain")
bool FFlockRelayTravelSeatSaysItAgainTest::RunTest(const FString& Parameters)
{
	FRelayTravelFixture F;
	const TSharedPtr<FFlockRelayConnection> Host = F.Device(*this, PlayerA);
	if (!Host.IsValid())
	{
		return false;
	}
	UFlockMultiplayerSession* Session = F.PlayerOf(*this, F.RelayConnectionOf(Host->GetAddress(), F.RelayServer()));
	const TSharedPtr<FFlockRelayConnection> Mine = F.Device(*this, PlayerB, Host->GetAddress());
	if (Session == nullptr || !Mine.IsValid())
	{
		return false;
	}
	// Its seat as a device from before this version left it: the relay address, nothing more.
	F.Server->SeatSaysRelayAddress(F.Served, PlayerB, Mine->GetAddress().ToString());
	F.ReadsTheSession(Session);
	const auto Publishes = [&]()
	{
		bool bAnswered = false;
		F.Sessions().PublishSeatRelayAddress(Session, Mine.ToSharedRef(), [&bAnswered](TFlockResult<UFlockMultiplayerSession*>) { bAnswered = true; });
		F.Until([&bAnswered]() { return bAnswered; });
	};
	Publishes();
	TestEqual(TEXT("published, to say its bridge reads the messages"), F.Server->Count(ERoute::PublishSeat), 1);
	// Its seat saying both now: not again.
	F.ReadsTheSession(Session);
	Publishes();
	TestEqual(TEXT("not published again"), F.Server->Count(ERoute::PublishSeat), 1);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
