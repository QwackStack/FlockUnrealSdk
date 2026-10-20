// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Misc/FlockEngineCompat.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Multiplayer/Relay/FlockRelayBridge.h"
#include "Multiplayer/Relay/FlockRelayThreads.h"
#include "Tests/Support/FlockRelayFixture.h"

// The relay bridge between real relay connections on a fake relay (FFlockFakeRelayServer) and plain loopback sockets standing in
// for the engine's net driver: a player's bridge standing in for the host, a host's bridge giving each relayed player a loopback
// port of its own, channels only for players let in, the places strangers can hold, and every socket and thread given back.

#define FLOCK_BRIDGE_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Relay.Bridge." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockRelayBridgeTestHelpers
{
	using namespace FlockRelayTestHelpers;

	struct FBridgeFixture : FRelayFixture
	{
		TArray<FFlockRelayBridge::FPtr> Bridges;

		~FBridgeFixture()
		{
			for (const FFlockRelayBridge::FPtr& Bridge : Bridges)
			{
				Bridge->Close();
			}
			Bridges.Reset();
			FFlockRelayBridge::MostPlayersForTesting = 0;
			FFlockRelayBridge::NotLetInWithinForTesting = 0.0;
			FFlockRelayThreads::bRefusedForTesting = false;
			FFlockRelayBridge::bSocketRefusedForTesting = false;
		}

		FFlockRelayBridge::FPtr Kept(FAutomationTestBase& Test, const TFlockRelayResult<FFlockRelayBridge::FPtr>& Made)
		{
			Test.TestTrue(FString::Printf(TEXT("precondition: the bridge was made (%s)"), *Made.Failure.Error.Message), Made.bSuccess && Made.Value.IsValid());
			if (Made.Value.IsValid())
			{
				Bridges.Add(Made.Value);
			}
			return Made.Value;
		}

		FFlockRelayBridge::FPtr ToTheHost(FAutomationTestBase& Test, const TSharedPtr<FFlockRelayConnection>& Through, const FFlockRelayPeer& Host)
		{
			return Kept(Test, FFlockRelayBridge::ToTheHost(Through.ToSharedRef(), Host, Log));
		}

		FFlockRelayBridge::FPtr ForTheHost(FAutomationTestBase& Test, const TSharedPtr<FFlockRelayConnection>& Through, int32 NetDriverPort)
		{
			return Kept(Test, FFlockRelayBridge::ForTheHost(Through.ToSharedRef(), NetDriverPort, Log));
		}

		/** Another player's relay address, opened to the host's. */
		TSharedPtr<FFlockRelayConnection> AnotherPlayer(FAutomationTestBase& Test, const FString& Player, const TSharedPtr<FFlockRelayConnection>& Host)
		{
			const TSharedPtr<FFlockRelayConnection> Opened = this->Opened(Test, Player);
			if (!Opened.IsValid())
			{
				return nullptr;
			}
			const TSharedRef<FOpenedTo> Open = OpenTo(Opened, Host->GetAddress());
			Until([&Open]() { return Open->bAnswered; });
			return Open->Result.bSuccess ? Opened : nullptr;
		}

		/** Runs frames until Engine heard Text, or Seconds pass. */
		bool EngineHears(FEngineSocket& Engine, const FString& Text, double Seconds = 3.0)
		{
			return Until([&]()
			{
				Engine.ReadEverything();
				return Engine.PortOf(Text) != 0;
			}, Seconds);
		}

		/** Runs frames for Seconds, reading what Engine hears. */
		void EngineListens(FEngineSocket& Engine, double Seconds)
		{
			Until([&]()
			{
				Engine.ReadEverything();
				return false;
			}, Seconds);
		}

		bool Sends(const TSharedPtr<FFlockRelayConnection>& From, const FFlockRelayPeer& To, const FString& Text)
		{
			const TArray<uint8> Payload = Bytes(Text);
			return From->Send(To, Payload.GetData(), Payload.Num());
		}
	};
}

using namespace FlockRelayBridgeTestHelpers;

// ---- a player's bridge ----

FLOCK_BRIDGE_TEST(FFlockBridgePlayerBothWaysTest, "APlayersBridgeCarriesTheEnginesPacketsToTheHostAndBack")
bool FFlockBridgePlayerBothWaysTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.ToTheHost(*this, Player, Host->GetAddress());
	FEngineSocket Engine;
	if (!Bridge.IsValid() || !TestTrue(TEXT("precondition: an engine socket"), Engine.Port > 0))
	{
		return false;
	}
	TestTrue(TEXT("the game travels to a loopback port"), Bridge->GetLocalPort() > 0);
	TestTrue(TEXT("the engine sent to the bridge"), Engine.SendTo(Bridge->GetLocalPort(), TEXT("hello from the engine")));
	TestTrue(TEXT("the host's relay address heard it"), F.Arrives(Host, TEXT("hello from the engine"), 3.0));
	TestTrue(TEXT("sent back"), F.Sends(Host, Player->GetAddress(), TEXT("hello from the host")));
	TestTrue(TEXT("the engine heard the host"), F.EngineHears(Engine, TEXT("hello from the host")));
	TestEqual(TEXT("from the port it travelled to, as from the host itself"), Engine.PortOf(TEXT("hello from the host")), Bridge->GetLocalPort());
	TestTrue(TEXT("the host gets a channel at once"), Player->KeepsChannelToForTesting(Host->GetAddress()));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgePlayerHostOnlyTest, "APlayersBridgeDropsWhatAnyoneButTheHostSends")
bool FFlockBridgePlayerHostOnlyTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	// A stranger on the relay, on the IP the player opened to.
	const TSharedPtr<FFlockRelayConnection> Stranger = F.AnotherPlayer(*this, TEXT("player-stranger"), Host);
	const FFlockRelayBridge::FPtr Bridge = F.ToTheHost(*this, Player, Host->GetAddress());
	FEngineSocket Engine;
	if (!Stranger.IsValid() || !Bridge.IsValid())
	{
		return false;
	}
	Engine.SendTo(Bridge->GetLocalPort(), TEXT("first"));
	F.Arrives(Host, TEXT("first"), 3.0);
	TestTrue(TEXT("the stranger sent to the player's relay address"), F.Sends(Stranger, Player->GetAddress(), TEXT("from a stranger")));
	TestTrue(TEXT("the host sent too"), F.Sends(Host, Player->GetAddress(), TEXT("from the host")));
	TestTrue(TEXT("the host is heard"), F.EngineHears(Engine, TEXT("from the host")));
	F.EngineListens(Engine, 0.3);
	TestEqual(TEXT("the stranger never reached the engine"), Engine.PortOf(TEXT("from a stranger")), 0);
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgePlayerFollowsTest, "APlayersBridgeFollowsItsEngineToANewPort")
bool FFlockBridgePlayerFollowsTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.ToTheHost(*this, Player, Host->GetAddress());
	FEngineSocket First;
	FEngineSocket Next;
	if (!Bridge.IsValid())
	{
		return false;
	}
	First.SendTo(Bridge->GetLocalPort(), TEXT("from the first socket"));
	TestTrue(TEXT("the first socket reached the host"), F.Arrives(Host, TEXT("from the first socket"), 3.0));
	// The net driver connects again from a new port each time (measured), so the newest port is the one.
	Next.SendTo(Bridge->GetLocalPort(), TEXT("from the next socket"));
	TestTrue(TEXT("the next socket reached the host"), F.Arrives(Host, TEXT("from the next socket"), 3.0));
	F.Sends(Host, Player->GetAddress(), TEXT("to the engine"));
	TestTrue(TEXT("the host's answer reached the port the engine sent from last"), F.EngineHears(Next, TEXT("to the engine")));
	F.EngineListens(First, 0.2);
	TestEqual(TEXT("not the old one"), First.PortOf(TEXT("to the engine")), 0);
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeLoopbackOnlyTest, "ABridgesLoopbackSocket_HearsNothingFromTheNetwork")
bool FFlockBridgeLoopbackOnlyTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	const FString Lan = FFlockDirectAddresses::LanAddressToward(TEXT("8.8.8.8"));
	if (Lan.IsEmpty() || Lan == FFlockUdpSockets::LoopbackIpv4)
	{
		AddInfo(TEXT("This machine has no network address of its own to send from: the check needs one."));
		return true;
	}
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.ToTheHost(*this, Player, Host->GetAddress());
	FSocket* FromTheNetwork = FFlockUdpSockets::OpenIpv4(TEXT("Flock test sender on the network"));
	if (!Bridge.IsValid() || !TestNotNull(TEXT("precondition: a socket on the network"), FromTheNetwork))
	{
		return false;
	}
	const TArray<uint8> Bytes = FFlockRelayWire::Utf8(TEXT("from the network"));
	int32 Sent = 0;
	FromTheNetwork->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *FFlockUdpSockets::Toward(*FromTheNetwork, Lan, Bridge->GetLocalPort()));
	FFlockUdpSockets::Close(FromTheNetwork);
	TestFalse(TEXT("a packet sent to the bridge's port at the device's network address goes nowhere"), F.Arrives(Host, TEXT("from the network"), 0.5));
	FEngineSocket Engine;
	Engine.SendTo(Bridge->GetLocalPort(), TEXT("from this device"));
	TestTrue(TEXT("one from this device's loopback is carried"), F.Arrives(Host, TEXT("from this device"), 3.0));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeDroppedTest, "ABridgeDroppedWithoutClosing_HandsArrivalsBack_EndsItsThreads_AndLetsItsChannelsGo")
bool FFlockBridgeDroppedTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const int32 ThreadsBefore = FFlockRelayThreads::CountForTesting();
	FEngineSocket Engine;
	{
		const TFlockRelayResult<FFlockRelayBridge::FPtr> Made = FFlockRelayBridge::ForTheHost(Host.ToSharedRef(), Engine.Port, F.Log);
		if (!TestTrue(TEXT("precondition: a host's bridge"), Made.bSuccess && Made.Value.IsValid()))
		{
			return false;
		}
		F.Sends(Player, Host->GetAddress(), TEXT("first"));
		F.EngineHears(Engine, TEXT("first"));
		Made.Value->LetIn(Player->GetAddress());
		TestTrue(TEXT("precondition: a channel to the player let in"), Host->KeepsChannelToForTesting(Player->GetAddress()));
	}
	// Its owner dropped it without closing it.
	F.Until([&]() { return !Host->HandsArrivalsOver() && FFlockRelayThreads::CountForTesting() == ThreadsBefore; }, 3.0);
	TestFalse(TEXT("the relay's arrivals go back to the relay"), Host->HandsArrivalsOver());
	TestEqual(TEXT("its loopback threads ended"), FFlockRelayThreads::CountForTesting(), ThreadsBefore);
	TestFalse(TEXT("its channels let go"), Host->KeepsChannelToForTesting(Player->GetAddress()));
	F.Sends(Player, Host->GetAddress(), TEXT("after"));
	TestTrue(TEXT("what arrives now waits for the relay's own reader"), F.Arrives(Host, TEXT("after"), 3.0));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeSecondRefusedTest, "ASecondBridgeOnOneRelayIsRefused_AndTheFirstKeepsCarrying")
bool FFlockBridgeSecondRefusedTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr First = F.ToTheHost(*this, Player, Host->GetAddress());
	const TFlockRelayResult<FFlockRelayBridge::FPtr> Second = FFlockRelayBridge::ToTheHost(Player.ToSharedRef(), Host->GetAddress(), F.Log);
	TestFalse(TEXT("two games behind one relay address would reach the host as one: refused"), Second.bSuccess);
	TestEqual(TEXT("a mistake, not a relay failure"), static_cast<int32>(Second.Failure.Error.Type), static_cast<int32>(EFlockErrorType::Validation));
	TestTrue(TEXT("with no relay reason"), Second.Failure.Reason.IsEmpty());
	TestFalse(TEXT("and a fix"), Second.Failure.Error.Hint.IsEmpty());
	FEngineSocket Engine;
	if (!First.IsValid())
	{
		return false;
	}
	Engine.SendTo(First->GetLocalPort(), TEXT("still carried"));
	TestTrue(TEXT("the first bridge still carries"), F.Arrives(Host, TEXT("still carried"), 3.0));
	TestTrue(TEXT("and keeps its channel"), Player->KeepsChannelToForTesting(Host->GetAddress()));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeNoThreadTest, "ABridgeWhoseThreadCannotStart_FailsRelayNoThread_AndAHostsBridgeDropsThePeer")
bool FFlockBridgeNoThreadTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FFlockRelayThreads::bRefusedForTesting = true;
	const TFlockRelayResult<FFlockRelayBridge::FPtr> Made = FFlockRelayBridge::ToTheHost(Player.ToSharedRef(), Host->GetAddress(), F.Log);
	TestFalse(TEXT("no bridge without its thread"), Made.bSuccess);
	TestEqualSensitive(TEXT("relay_no_thread"), Made.Failure.Reason, FString(FlockRelayFailureReasons::NoThread));
	TestFalse(TEXT("with the reason's fix"), Made.Failure.Error.Hint.IsEmpty());
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr OnTheHost = F.ForTheHost(*this, Host, Engine.Port);
	if (!OnTheHost.IsValid())
	{
		return false;
	}
	F.Sends(Player, Host->GetAddress(), TEXT("no place for it"));
	F.EngineListens(Engine, 0.3);
	TestEqual(TEXT("a peer whose socket cannot get its thread gets no place"), OnTheHost->GetPeersForTesting(), 0);
	TestEqual(TEXT("and nothing reached the engine"), Engine.PortOf(TEXT("no place for it")), 0);
	FFlockRelayThreads::bRefusedForTesting = false;
	F.Sends(Player, Host->GetAddress(), TEXT("its resend"));
	TestTrue(TEXT("its next packet gets the place"), F.EngineHears(Engine, TEXT("its resend")));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeNoSocketTest, "ABridgeWithNoLoopbackSocket_FailsWithItsFix")
bool FFlockBridgeNoSocketTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FFlockRelayBridge::bSocketRefusedForTesting = true;
	const TFlockRelayResult<FFlockRelayBridge::FPtr> Made = FFlockRelayBridge::ToTheHost(Player.ToSharedRef(), Host->GetAddress(), F.Log);
	TestFalse(TEXT("no bridge without its socket"), Made.bSuccess);
	TestEqual(TEXT("a network failure"), static_cast<int32>(Made.Failure.Error.Type), static_cast<int32>(EFlockErrorType::Network));
	TestTrue(TEXT("naming the loopback"), Made.Failure.Error.Message.Contains(TEXT("127.0.0.1")));
	TestTrue(TEXT("with its own fix"), Made.Failure.Error.Hint.Contains(TEXT("firewall")) && Made.Failure.Error.Hint.Contains(TEXT("127.0.0.1")));
	TestFalse(TEXT("the relay's arrivals are not taken by a bridge that was not made"), Player->HandsArrivalsOver());
	return true;
}

// ---- a host's bridge ----

FLOCK_BRIDGE_TEST(FFlockBridgeHostPortsTest, "AHostsBridgeGivesEachRelayedPeerALoopbackPortOfItsOwn_BothWays")
bool FFlockBridgeHostPortsTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> First;
	if (!F.HostAndPlayer(*this, Host, First))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Second = F.AnotherPlayer(*this, TEXT("player-c"), Host);
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Second.IsValid() || !Bridge.IsValid())
	{
		return false;
	}
	F.Sends(First, Host->GetAddress(), TEXT("from the first player"));
	F.Sends(Second, Host->GetAddress(), TEXT("from the second player"));
	TestTrue(TEXT("the engine heard the first"), F.EngineHears(Engine, TEXT("from the first player")));
	TestTrue(TEXT("and the second"), F.EngineHears(Engine, TEXT("from the second player")));
	const int32 FirstPort = Engine.PortOf(TEXT("from the first player"));
	const int32 SecondPort = Engine.PortOf(TEXT("from the second player"));
	TestNotEqual(TEXT("each from a loopback port of its own, as the engine needs to tell connections apart"), FirstPort, SecondPort);
	FFlockRelayPeer Behind;
	TestTrue(TEXT("the first port is the first player's"), Bridge->TryGetPeer(FirstPort, Behind) && Behind == First->GetAddress());
	TestTrue(TEXT("the second port is the second player's"), Bridge->TryGetPeer(SecondPort, Behind) && Behind == Second->GetAddress());
	TestEqual(TEXT("and the other way"), Bridge->LoopbackPortOf(Second->GetAddress()), SecondPort);
	TestFalse(TEXT("a port of no peer is none"), Bridge->TryGetPeer(Engine.Port, Behind));
	Engine.SendTo(FirstPort, TEXT("to the first player"));
	Engine.SendTo(SecondPort, TEXT("to the second player"));
	TestTrue(TEXT("the first player got the engine's packet"), F.Arrives(First, TEXT("to the first player"), 3.0));
	TestTrue(TEXT("the second player got its own"), F.Arrives(Second, TEXT("to the second player"), 3.0));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeHostOnlyTheDriverTest, "AHostsBridgeSendsOnOnlyWhatTheNetDriverSends")
bool FFlockBridgeHostOnlyTheDriverTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FEngineSocket Engine;
	FEngineSocket Other;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Bridge.IsValid())
	{
		return false;
	}
	F.Sends(Player, Host->GetAddress(), TEXT("hello"));
	F.EngineHears(Engine, TEXT("hello"));
	const int32 PeerPort = Engine.PortOf(TEXT("hello"));
	// The net driver's first, so waiting for it reads nothing the other program sent.
	Engine.SendTo(PeerPort, TEXT("from the net driver"));
	TestTrue(TEXT("the net driver's packet went on"), F.Arrives(Player, TEXT("from the net driver"), 3.0));
	Other.SendTo(PeerPort, TEXT("from another program"));
	TestFalse(TEXT("another program's did not"), F.Arrives(Player, TEXT("from another program"), 0.5));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeHostFollowsTest, "AHostsBridgeFollowsTheNetDriverToANewPort")
bool FFlockBridgeHostFollowsTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FEngineSocket Before;
	FEngineSocket After;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Before.Port);
	if (!Bridge.IsValid())
	{
		return false;
	}
	F.Sends(Player, Host->GetAddress(), TEXT("before"));
	TestTrue(TEXT("reached the level listening"), F.EngineHears(Before, TEXT("before")));
	const int32 PeerPort = Before.PortOf(TEXT("before"));
	// The host's level opened again (its own level change) on another port.
	Bridge->SetNetDriverPort(After.Port);
	Bridge->SetNetDriverPort(0);
	TestEqual(TEXT("a port of nothing is not taken"), Bridge->GetNetDriverPortForTesting(), After.Port);
	F.Sends(Player, Host->GetAddress(), TEXT("after"));
	TestTrue(TEXT("reached the level listening now"), F.EngineHears(After, TEXT("after")));
	F.EngineListens(Before, 0.2);
	TestEqual(TEXT("not the old one"), Before.PortOf(TEXT("after")), 0);
	TestEqual(TEXT("through the same loopback port"), After.PortOf(TEXT("after")), PeerPort);
	Before.SendTo(PeerPort, TEXT("from the old socket"));
	After.SendTo(PeerPort, TEXT("from the net driver now"));
	TestTrue(TEXT("the net driver listening now is carried"), F.Arrives(Player, TEXT("from the net driver now"), 3.0));
	TestFalse(TEXT("the old port is not"), F.Arrives(Player, TEXT("from the old socket"), 0.3));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeChannelOnLetInTest, "AHostsBridgeBindsAChannel_OnlyToAPeerLetIn")
bool FFlockBridgeChannelOnLetInTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Bridge.IsValid())
	{
		return false;
	}
	F.Sends(Player, Host->GetAddress(), TEXT("hello"));
	F.EngineHears(Engine, TEXT("hello"));
	TestFalse(TEXT("a peer only heard gets no channel: a stranger cannot take the 64"), Host->KeepsChannelToForTesting(Player->GetAddress()));
	Bridge->LetIn(Player->GetAddress());
	TestTrue(TEXT("one the host's check let in does"), Host->KeepsChannelToForTesting(Player->GetAddress()));
	Bridge->LetIn(FFlockRelayPeer(0x7F000001, 9));
	TestFalse(TEXT("a peer the bridge never heard gets none"), Host->KeepsChannelToForTesting(FFlockRelayPeer(0x7F000001, 9)));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeForgetTest, "AHostsBridgeForgetsAPeer_StopsItsChannel_AndGivesItANewPortWhenItSendsAgain")
bool FFlockBridgeForgetTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Bridge.IsValid())
	{
		return false;
	}
	const int32 ThreadsBefore = FFlockRelayThreads::CountForTesting();
	F.Sends(Player, Host->GetAddress(), TEXT("first"));
	F.EngineHears(Engine, TEXT("first"));
	const int32 FirstPort = Engine.PortOf(TEXT("first"));
	Bridge->LetIn(Player->GetAddress());
	Bridge->Forget(Player->GetAddress());
	TestEqual(TEXT("its place given back"), Bridge->GetPeersForTesting(), 0);
	TestFalse(TEXT("its channel no longer renewed"), Host->KeepsChannelToForTesting(Player->GetAddress()));
	F.Until([&]() { return FFlockRelayThreads::CountForTesting() == ThreadsBefore; }, 3.0);
	TestEqual(TEXT("its loopback thread ended"), FFlockRelayThreads::CountForTesting(), ThreadsBefore);
	F.Sends(Player, Host->GetAddress(), TEXT("again"));
	TestTrue(TEXT("heard again"), F.EngineHears(Engine, TEXT("again")));
	TestNotEqual(TEXT("from a new loopback port"), Engine.PortOf(TEXT("again")), FirstPort);
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeNoLongerLetInTest, "APeerNoLongerLetIn_KeepsItsPort_ButNotItsChannel")
bool FFlockBridgeNoLongerLetInTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Bridge.IsValid())
	{
		return false;
	}
	F.Sends(Player, Host->GetAddress(), TEXT("first"));
	F.EngineHears(Engine, TEXT("first"));
	const int32 Port = Engine.PortOf(TEXT("first"));
	Bridge->LetIn(Player->GetAddress());
	Bridge->NoLongerLetIn(Player->GetAddress());
	TestEqual(TEXT("its port kept, for a connection still coming through it"), Bridge->LoopbackPortOf(Player->GetAddress()), Port);
	TestFalse(TEXT("its channel let go until it is let in again"), Host->KeepsChannelToForTesting(Player->GetAddress()));
	F.Sends(Player, Host->GetAddress(), TEXT("again"));
	TestTrue(TEXT("heard again"), F.EngineHears(Engine, TEXT("again")));
	TestEqual(TEXT("through the same port"), Engine.PortOf(TEXT("again")), Port);
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeNeverLetInTest, "AHostsBridgeForgetsAPeerNeverLetIn_WhenANewPeerArrivesAfterItsTime")
bool FFlockBridgeNeverLetInTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Stranger;
	if (!F.HostAndPlayer(*this, Host, Stranger))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> LetIn = F.AnotherPlayer(*this, TEXT("player-c"), Host);
	const TSharedPtr<FFlockRelayConnection> Later = F.AnotherPlayer(*this, TEXT("player-d"), Host);
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!LetIn.IsValid() || !Later.IsValid() || !Bridge.IsValid())
	{
		return false;
	}
	FFlockRelayBridge::NotLetInWithinForTesting = 0.3;
	const int32 ThreadsBefore = FFlockRelayThreads::CountForTesting();
	F.Sends(Stranger, Host->GetAddress(), TEXT("a stranger"));
	F.Sends(LetIn, Host->GetAddress(), TEXT("a player"));
	F.EngineHears(Engine, TEXT("a stranger"));
	F.EngineHears(Engine, TEXT("a player"));
	Bridge->LetIn(LetIn->GetAddress());
	F.RealSeconds(0.5);
	F.Sends(Later, Host->GetAddress(), TEXT("a new peer"));
	TestTrue(TEXT("the new peer is heard"), F.EngineHears(Engine, TEXT("a new peer")));
	TestEqual(TEXT("the stranger never let in gave its place back"), Bridge->LoopbackPortOf(Stranger->GetAddress()), 0);
	TestTrue(TEXT("the player let in kept its own"), Bridge->LoopbackPortOf(LetIn->GetAddress()) > 0);
	TestEqual(TEXT("two places held"), Bridge->GetPeersForTesting(), 2);
	F.Until([&]() { return FFlockRelayThreads::CountForTesting() == ThreadsBefore + 2; }, 3.0);
	TestEqual(TEXT("the stranger's loopback thread ended: one each for the two held"), FFlockRelayThreads::CountForTesting(), ThreadsBefore + 2);
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeFullTest, "AFullHostsBridge_MakesWayOnlyForAPeerNeverLetIn_OnceItHadItsTime")
bool FFlockBridgeFullTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> First;
	if (!F.HostAndPlayer(*this, Host, First))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Second = F.AnotherPlayer(*this, TEXT("player-c"), Host);
	const TSharedPtr<FFlockRelayConnection> Third = F.AnotherPlayer(*this, TEXT("player-d"), Host);
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Second.IsValid() || !Third.IsValid() || !Bridge.IsValid())
	{
		return false;
	}
	FFlockRelayBridge::MostPlayersForTesting = 2;
	FFlockRelayBridge::NotLetInWithinForTesting = 0.3;
	F.Sends(First, Host->GetAddress(), TEXT("first"));
	F.Sends(Second, Host->GetAddress(), TEXT("second"));
	F.EngineHears(Engine, TEXT("first"));
	F.EngineHears(Engine, TEXT("second"));
	Bridge->LetIn(First->GetAddress());
	F.Sends(Third, Host->GetAddress(), TEXT("third, too early"));
	F.EngineListens(Engine, 0.2);
	TestEqual(TEXT("a full bridge takes no third while the others are within their time"), Engine.PortOf(TEXT("third, too early")), 0);
	F.RealSeconds(0.4);
	F.Sends(Third, Host->GetAddress(), TEXT("third, in time"));
	TestTrue(TEXT("once the one never let in had its time, it makes way"), F.EngineHears(Engine, TEXT("third, in time")));
	TestEqual(TEXT("the second, never let in, is gone"), Bridge->LoopbackPortOf(Second->GetAddress()), 0);
	TestTrue(TEXT("the first, let in, stays"), Bridge->LoopbackPortOf(First->GetAddress()) > 0);
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeTooLargeTest, "APacketLargerThanTheRelayCarries_IsDropped_WithOneWarning")
bool FFlockBridgeTooLargeTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const FFlockRelayBridge::FPtr Bridge = F.ToTheHost(*this, Player, Host->GetAddress());
	FEngineSocket Engine;
	if (!Bridge.IsValid())
	{
		return false;
	}
	TArray<uint8> TooLarge;
	TooLarge.Init(static_cast<uint8>('x'), FFlockRelayConnection::MostBytesInAPacket + 1);
	TArray<uint8> Largest;
	Largest.Init(static_cast<uint8>('y'), FFlockRelayConnection::MostBytesInAPacket);
	Engine.SendTo(Bridge->GetLocalPort(), TooLarge);
	Engine.SendTo(Bridge->GetLocalPort(), TooLarge);
	Engine.SendTo(Bridge->GetLocalPort(), Largest);
	uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
	int32 Count = 0;
	FFlockRelayPeer From;
	bool bLargestHeard = false;
	bool bTooLargeHeard = false;
	F.Until([&]()
	{
		while (Host->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From))
		{
			bLargestHeard = bLargestHeard || (Count == FFlockRelayConnection::MostBytesInAPacket && Buffer[0] == 'y');
			bTooLargeHeard = bTooLargeHeard || Buffer[0] == 'x';
		}
		return bLargestHeard;
	}, 3.0);
	TestTrue(TEXT("the largest packet the relay carries is carried"), bLargestHeard);
	TestFalse(TEXT("a larger one is not, nor cut to fit"), bTooLargeHeard);
	Bridge->Tick();
	// Another, after the warning: still told once in all.
	Engine.SendTo(Bridge->GetLocalPort(), TooLarge);
	Engine.SendTo(Bridge->GetLocalPort(), Largest);
	bLargestHeard = false;
	F.Until([&]()
	{
		while (Host->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From))
		{
			bLargestHeard = bLargestHeard || (Count == FFlockRelayConnection::MostBytesInAPacket && Buffer[0] == 'y');
		}
		return bLargestHeard;
	}, 3.0);
	Bridge->Tick();
	TestEqual(TEXT("told once"), F.Log->Warnings.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("larger than the 1400 bytes")); }).Num(), 1);
	return true;
}

// ---- giving everything back ----

FLOCK_BRIDGE_TEST(FFlockBridgeCloseTest, "CloseClosesEveryLoopbackSocket_EndsItsThreads_AndHandsArrivalsBackToTheRelay")
bool FFlockBridgeCloseTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const int32 ThreadsBefore = FFlockRelayThreads::CountForTesting();
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr Bridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Bridge.IsValid())
	{
		return false;
	}
	F.Sends(Player, Host->GetAddress(), TEXT("before"));
	F.EngineHears(Engine, TEXT("before"));
	const int32 PeerPort = Engine.PortOf(TEXT("before"));
	TestTrue(TEXT("a loopback thread for the peer"), FFlockRelayThreads::CountForTesting() > ThreadsBefore);
	TestTrue(TEXT("the bridge hears what arrives"), Host->HandsArrivalsOver());
	Bridge->Close();
	TestTrue(TEXT("closed"), Bridge->IsClosed());
	TestFalse(TEXT("arrivals handed back to the relay"), Host->HandsArrivalsOver());
	F.Until([&]() { return FFlockRelayThreads::CountForTesting() == ThreadsBefore; }, 3.0);
	TestEqual(TEXT("every loopback thread ended"), FFlockRelayThreads::CountForTesting(), ThreadsBefore);
	F.Sends(Player, Host->GetAddress(), TEXT("after"));
	TestTrue(TEXT("what arrives now waits for the relay's own reader"), F.Arrives(Host, TEXT("after"), 3.0));
	F.EngineListens(Engine, 0.2);
	TestEqual(TEXT("and never reached the engine"), Engine.PortOf(TEXT("after")), 0);
	Engine.SendTo(PeerPort, TEXT("to a closed port"));
	TestFalse(TEXT("the peer's loopback socket carries nothing more"), F.Arrives(Player, TEXT("to a closed port"), 0.3));
	return true;
}

FLOCK_BRIDGE_TEST(FFlockBridgeChannelsLetGoTest, "AClosedBridge_StopsRenewingTheChannelsItKept")
bool FFlockBridgeChannelsLetGoTest::RunTest(const FString& Parameters)
{
	FBridgeFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Second = F.AnotherPlayer(*this, TEXT("player-c"), Host);
	const FFlockRelayBridge::FPtr PlayersBridge = F.ToTheHost(*this, Player, Host->GetAddress());
	FEngineSocket Engine;
	const FFlockRelayBridge::FPtr HostsBridge = F.ForTheHost(*this, Host, Engine.Port);
	if (!Second.IsValid() || !PlayersBridge.IsValid() || !HostsBridge.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("precondition: the player keeps a channel to its host"), Player->KeepsChannelToForTesting(Host->GetAddress()));
	PlayersBridge->Close();
	TestFalse(TEXT("a closed player's bridge stops renewing its channel to the host"), Player->KeepsChannelToForTesting(Host->GetAddress()));

	F.Sends(Second, Host->GetAddress(), TEXT("hello"));
	F.EngineHears(Engine, TEXT("hello"));
	HostsBridge->LetIn(Second->GetAddress());
	TestTrue(TEXT("precondition: the host keeps a channel to its player"), Host->KeepsChannelToForTesting(Second->GetAddress()));
	HostsBridge->Close();
	TestFalse(TEXT("a closed host's bridge stops renewing its players' channels"), Host->KeepsChannelToForTesting(Second->GetAddress()));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
