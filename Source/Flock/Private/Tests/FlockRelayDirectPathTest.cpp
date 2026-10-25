// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/FlockEngineCompat.h"
#include "Multiplayer/FlockNameLookup.h"
#include "Multiplayer/FlockSockets.h"
#include "Multiplayer/Relay/FlockBridgeMessages.h"
#include "Multiplayer/Relay/FlockDirectLink.h"
#include "Multiplayer/Relay/FlockRelayBridge.h"
#include "Tests/Support/FlockFakeStunServer.h"
#include "Tests/Support/FlockRelayFixture.h"

// Two bridges that go direct when they can, on real loopback sockets with a fake relay (which, like coturn and Cloudflare's,
// answers a STUN request with the address it saw it from) and plain loopback sockets standing in for each end's net driver: the
// engine's packets move off the relay once a probe is answered, and back when the direct path goes quiet; a host offers its
// addresses only to players its check let in whose seats say they read the bridges' messages, and only probes carrying both keys
// count; the bridges' own messages never reach the engine; a new connection through a relay address an earlier one came through
// reaches the engine from a loopback port of its own.

#define FLOCK_DIRECT_PATH_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Relay.Bridge.DirectPath." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockDirectPathTestHelpers
{
	using namespace FlockRelayTestHelpers;

	FFlockDirectLink::FTiming QuickLinks()
	{
		FFlockDirectLink::FTiming Timing;
		Timing.TickSeconds = 0.02;
		Timing.AskAddressEverySeconds = 0.1;
		Timing.AddressAsks = 2;
		Timing.OfferEverySeconds = 0.2;
		Timing.OfferForSeconds = 3.0;
		Timing.ProbeEverySeconds = 0.05;
		Timing.ProbeForSeconds = 1.5;
		Timing.KeepAliveEverySeconds = 0.2;
		Timing.QuietForSeconds = 0.8;
		return Timing;
	}

	FFlockBridgeKey Key(uint8 First, uint8 Step = 1)
	{
		FFlockBridgeKey Made;
		for (int32 Index = 0; Index < FFlockBridgeKey::Length; ++Index)
		{
			Made.Bytes[Index] = static_cast<uint8>(First + Index * Step);
		}
		return Made;
	}

	FFlockBridgeKey Repeated(uint8 Byte) { return Key(Byte, 0); }

	struct FPairOptions
	{
		bool bHostGoesDirect = true;
		bool bPlayerGoesDirect = true;
		bool bLetIn = true;
		/** The host published that its bridge reads the bridges' messages. */
		bool bHostReadsBridgeMessages = true;
		/** The player's seat says its bridge reads them. */
		bool bPlayerReadsBridgeMessages = true;
	};

	/** A host's bridge and a player's on one relay, the player's engine heard by the host's. */
	struct FPair
	{
		TSharedPtr<FFlockRelayConnection> HostRelay;
		TSharedPtr<FFlockRelayConnection> PlayerRelay;
		FFlockRelayBridge::FPtr Host;
		FFlockRelayBridge::FPtr Player;
		TUniquePtr<FEngineSocket> HostEngine;
		TUniquePtr<FEngineSocket> PlayerEngine;
		/** The loopback port the host's engine hears the player from. */
		int32 PlayersPortAtTheHost = 0;

		FFlockRelayPeer PlayersAddress() const { return PlayerRelay->GetAddress(); }
		FFlockRelayPeer HostsAddress() const { return HostRelay->GetAddress(); }
		FFlockDirectLink::FPtr HostsLink() const { return Host.IsValid() ? Host->GetDirectLinkForTesting(PlayersAddress()) : nullptr; }
		FFlockDirectLink::FPtr PlayersLink() const { return Player.IsValid() ? Player->GetDirectLinkForTesting(HostsAddress()) : nullptr; }
	};

	struct FDirectFixture : FRelayFixture
	{
		TArray<FFlockRelayBridge::FPtr> Bridges;
		TUniquePtr<FFlockFakeStunServer> Stun;

		FDirectFixture() { FFlockDirectLink::TimingForTesting = QuickLinks(); }

		~FDirectFixture()
		{
			for (const FFlockRelayBridge::FPtr& Bridge : Bridges)
			{
				Bridge->Close();
			}
			Bridges.Reset();
			FFlockRelayBridge::MostPlayersForTesting = 0;
			FFlockRelayBridge::NotLetInWithinForTesting = 0.0;
			FFlockDirectLink::TimingForTesting.Reset();
			FFlockDirectLink::OfferInsteadForTesting(TArray<FFlockRelayPeer>());
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

		/** Frames until Done or Seconds pass, each running the relays and the bridges as the game would, and the STUN server. */
		bool Settle(TFunctionRef<bool()> Done, double Seconds)
		{
			return Until([&]()
			{
				for (const FFlockRelayBridge::FPtr& Bridge : Bridges)
				{
					Bridge->Tick();
				}
				if (Stun.IsValid())
				{
					Stun->Serve();
				}
				return Done();
			}, Seconds);
		}

		void Seconds(double Seconds) { Settle([]() { return false; }, Seconds); }

		bool EngineHears(FEngineSocket& Engine, const FString& Text, double Seconds = 3.0)
		{
			return Settle([&]()
			{
				Engine.ReadEverything();
				return Engine.PortOf(Text) != 0;
			}, Seconds);
		}

		/** Opens both relay addresses (with BeforeBridges run once they are), then the bridges; the host lets the player in unless told not to. */
		bool Paired(FAutomationTestBase& Test, FPair& Pair, const FPairOptions& Options = FPairOptions(), TFunction<void()> BeforeBridges = nullptr)
		{
			if (!HostAndPlayer(Test, Pair.HostRelay, Pair.PlayerRelay))
			{
				return false;
			}
			if (BeforeBridges)
			{
				BeforeBridges();
			}
			Pair.HostEngine = MakeUnique<FEngineSocket>();
			Pair.PlayerEngine = MakeUnique<FEngineSocket>();
			Pair.Host = Kept(Test, FFlockRelayBridge::ForTheHost(Pair.HostRelay.ToSharedRef(), Pair.HostEngine->Port, Log, Options.bHostGoesDirect));
			FFlockRelayBridge::FToTheHostOptions Bridging;
			Bridging.bHostReadsBridgeMessages = Options.bHostReadsBridgeMessages;
			Bridging.bGoesDirect = Options.bPlayerGoesDirect;
			Pair.Player = Kept(Test, FFlockRelayBridge::ToTheHost(Pair.PlayerRelay.ToSharedRef(), Pair.HostsAddress(), Log, Bridging));
			if (!Pair.Host.IsValid() || !Pair.Player.IsValid())
			{
				return false;
			}
			if (Options.bPlayerReadsBridgeMessages)
			{
				// As the host's reading of the session hands it over: the player's seat says its bridge reads them.
				Pair.Host->SetPeersThatReadBridgeMessages({ Pair.PlayersAddress() });
			}
			Pair.PlayerEngine->SendTo(Pair.Player->GetLocalPort(), TEXT("hello"));
			if (!Test.TestTrue(TEXT("precondition: the host's engine heard the player"), EngineHears(*Pair.HostEngine, TEXT("hello"))))
			{
				return false;
			}
			Pair.PlayersPortAtTheHost = Pair.HostEngine->PortOf(TEXT("hello"));
			if (Options.bLetIn)
			{
				Pair.Host->LetIn(Pair.PlayersAddress());
			}
			return true;
		}

		bool BothDirect(FAutomationTestBase& Test, const FPair& Pair, double Seconds = 5.0)
		{
			return Test.TestTrue(TEXT("both ends went direct"), Settle([&]()
			{
				const FFlockDirectLink::FPtr Hosts = Pair.HostsLink();
				const FFlockDirectLink::FPtr Players = Pair.PlayersLink();
				return Hosts.IsValid() && Hosts->IsDirect() && Players.IsValid() && Players->IsDirect();
			}, Seconds));
		}

		/** One packet each way between the two engines, judged by what they hear and from where. */
		bool OneEachWay(FAutomationTestBase& Test, FPair& Pair, const FString& Word)
		{
			const FString ToHost = TEXT("to-host-") + Word;
			Pair.PlayerEngine->SendTo(Pair.Player->GetLocalPort(), ToHost);
			if (!Test.TestTrue(FString::Printf(TEXT("the host's engine heard \"%s\""), *ToHost), EngineHears(*Pair.HostEngine, ToHost)))
			{
				return false;
			}
			Test.TestEqual(TEXT("The host's engine hears the player from the port it always has"), Pair.HostEngine->PortOf(ToHost), Pair.PlayersPortAtTheHost);
			const FString ToPlayer = TEXT("to-player-") + Word;
			Pair.HostEngine->SendTo(Pair.PlayersPortAtTheHost, ToPlayer);
			if (!Test.TestTrue(FString::Printf(TEXT("the player's engine heard \"%s\""), *ToPlayer), EngineHears(*Pair.PlayerEngine, ToPlayer)))
			{
				return false;
			}
			Test.TestEqual(TEXT("The player's engine hears the host from the port it connected to"), Pair.PlayerEngine->PortOf(ToPlayer), Pair.Player->GetLocalPort());
			return true;
		}

		/** Channel frames and send indications the relay passed on from Connection's socket: what went through the relay. */
		int32 RelayedFrom(const TSharedPtr<FFlockRelayConnection>& Connection) const
		{
			const int32 Port = Connection->GetLocalPortForTesting();
			return Relay->AllSeen().FilterByPredicate([Port](const FFlockFakeRelayServer::FSeen& Seen)
			{
				return Seen.ClientPort == Port && (Seen.Method == FFlockFakeRelayServer::ChannelFrame
					|| (Seen.Method == FFlockFakeRelayServer::MethodSend && Seen.Class == 0x010));
			}).Num();
		}

		bool InfoLogged(const FString& Fragment) const { return FFlockRecordingLogger::AnyContains(Log->Infos, Fragment); }

		bool Sends(const TSharedPtr<FFlockRelayConnection>& From, const FFlockRelayPeer& To, const FString& Text)
		{
			const TArray<uint8> Payload = Bytes(Text);
			return From->Send(To, Payload.GetData(), Payload.Num());
		}

		/** Everything Receiver's relay address was sent through the relay, read without a bridge. */
		static TArray<TArray<uint8>> Received(const TSharedPtr<FFlockRelayConnection>& Receiver)
		{
			TArray<TArray<uint8>> Packets;
			uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
			int32 Count = 0;
			FFlockRelayPeer From;
			while (Receiver->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From))
			{
				Packets.Emplace(Buffer, Count);
			}
			return Packets;
		}

		/** A UDP socket on 127.0.0.1 that is no end of the pair, sending at a link's socket. */
		static void StrangerSends(FEngineSocket& Stranger, int32 ToPort, const TArray<uint8>& Bytes, int32 Times)
		{
			for (int32 Index = 0; Index < Times; ++Index)
			{
				Stranger.SendTo(ToPort, Bytes);
			}
		}
	};
}

using namespace FlockDirectPathTestHelpers;

// ---- the bridges' own messages ----

FLOCK_DIRECT_PATH_TEST(FFlockDirectOfferReadsBackTest, "Messages.AnOffer_ReadsBackAsWritten")
bool FFlockDirectOfferReadsBackTest::RunTest(const FString& Parameters)
{
	const FFlockBridgeKey Senders = Key(1);
	const FFlockBridgeKey Receivers = Key(9);
	const TArray<FFlockRelayPeer> Addresses = { Peer(TEXT("203.0.113.9:61000")), Peer(TEXT("192.168.1.4:5000")) };
	const TArray<uint8> Offer = FFlockBridgeMessages::WriteOffer(Senders, Receivers, Addresses);

	FFlockBridgeKey ReadSenders;
	TOptional<FFlockBridgeKey> ReadReceivers;
	TArray<FFlockRelayPeer> ReadAddresses;
	TestTrue(TEXT("read"), FFlockBridgeMessages::TryReadOffer(Offer.GetData(), Offer.Num(), ReadSenders, ReadReceivers, ReadAddresses));
	TestTrue(TEXT("the sender's key"), ReadSenders == Senders);
	TestTrue(TEXT("the receiver's key as the sender holds it"), ReadReceivers.IsSet() && ReadReceivers.GetValue() == Receivers);
	TestTrue(TEXT("the addresses, in order"), ReadAddresses == Addresses);

	const TArray<uint8> First = FFlockBridgeMessages::WriteOffer(Senders, TOptional<FFlockBridgeKey>(), Addresses);
	TestTrue(TEXT("read"), FFlockBridgeMessages::TryReadOffer(First.GetData(), First.Num(), ReadSenders, ReadReceivers, ReadAddresses));
	TestFalse(TEXT("Before the sender has heard the receiver's key"), ReadReceivers.IsSet());
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectMalformedOfferTest, "Messages.AShortOrMalformedOffer_IsNotRead")
bool FFlockDirectMalformedOfferTest::RunTest(const FString& Parameters)
{
	const FFlockBridgeKey Zeros;
	const TArray<uint8> Offer = FFlockBridgeMessages::WriteOffer(Zeros, TOptional<FFlockBridgeKey>(), { Peer(TEXT("127.0.0.1:7777")) });
	FFlockBridgeKey Senders;
	TOptional<FFlockBridgeKey> Receivers;
	TArray<FFlockRelayPeer> Addresses;
	TestTrue(TEXT("Precondition: whole, it reads"), FFlockBridgeMessages::TryReadOffer(Offer.GetData(), Offer.Num(), Senders, Receivers, Addresses));
	TestFalse(TEXT("Cut short"), FFlockBridgeMessages::TryReadOffer(Offer.GetData(), Offer.Num() - 1, Senders, Receivers, Addresses));
	TArray<uint8> NoPort = Offer;
	NoPort[NoPort.Num() - 1] = 0;
	NoPort[NoPort.Num() - 2] = 0;
	TestFalse(TEXT("Port 0"), FFlockBridgeMessages::TryReadOffer(NoPort.GetData(), NoPort.Num(), Senders, Receivers, Addresses));
	TestEqual(TEXT("a refused offer hands back no addresses"), Addresses.Num(), 0);
	TArray<uint8> TooMany = Offer;
	TooMany[FFlockBridgeMessages::HeaderLength + 2 * FFlockBridgeKey::Length] = FFlockBridgeMessages::MostAddressesOffered + 1;
	TestFalse(TEXT("More addresses than allowed"), FFlockBridgeMessages::TryReadOffer(TooMany.GetData(), TooMany.Num(), Senders, Receivers, Addresses));
	const TArray<uint8> Probe = FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Probe, Zeros, Zeros);
	TestFalse(TEXT("Another kind"), FFlockBridgeMessages::TryReadOffer(Probe.GetData(), Probe.Num(), Senders, Receivers, Addresses));
	TArray<uint8> Later = Offer;
	Later[4] = FFlockBridgeMessages::Version + 1;
	TestFalse(TEXT("A later version's"), FFlockBridgeMessages::TryReadOffer(Later.GetData(), Later.Num(), Senders, Receivers, Addresses));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectNoEnginePacketTest, "Messages.NoEnginePacket_ReadsAsTheBridgesOwnMessage")
bool FFlockDirectNoEnginePacketTest::RunTest(const FString& Parameters)
{
	// The engine's first bits: a 2-bit session id, a 3-bit client id, the handshake bit; a handshake packet then its restart bit and
	// its lowest and current handshake versions, a byte each (read in StatelessConnectHandlerComponent::BeginHandshakePacket).
	int32 Packets = 0;
	for (uint8 Session = 0; Session < 4; ++Session)
	{
		for (uint8 Client = 0; Client < 8; ++Client)
		{
			for (uint8 Restart = 0; Restart < 2; ++Restart)
			{
				for (uint8 Lowest = 0; Lowest < 16; ++Lowest)
				{
					for (uint8 Current = 0; Current < 16; ++Current)
					{
						const uint8 Handshake[] = { static_cast<uint8>(Session | (Client << 2) | (1 << 5) | (Restart << 6) | ((Lowest & 1) << 7)),
							static_cast<uint8>((Lowest >> 1) | ((Current & 1) << 7)), static_cast<uint8>('N'), static_cast<uint8>('K'), 1, 1 };
						Packets += FFlockBridgeMessages::IsBridgeMessage(Handshake, UE_ARRAY_COUNT(Handshake)) ? 1 : 0;
					}
				}
			}
			// A game packet has the handshake bit clear, whatever follows.
			const uint8 Game[] = { static_cast<uint8>(Session | (Client << 2) | 0xC0), static_cast<uint8>('L'), static_cast<uint8>('N'), static_cast<uint8>('K'), 1, 1 };
			Packets += FFlockBridgeMessages::IsBridgeMessage(Game, UE_ARRAY_COUNT(Game)) ? 1 : 0;
		}
	}
	TestEqual(TEXT("No packet of the engine's reads as one of the bridges' messages"), Packets, 0);
	const uint8 MarkerOnly[] = { FFlockBridgeMessages::Marker, 1, 2, 3, 4, 5 };
	TestFalse(TEXT("The marker without the letters"), FFlockBridgeMessages::IsBridgeMessage(MarkerOnly, UE_ARRAY_COUNT(MarkerOnly)));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectEveryVersionTest, "Messages.EveryMessage_OfAnyVersion_IsTheBridgesOwn")
bool FFlockDirectEveryVersionTest::RunTest(const FString& Parameters)
{
	const FFlockBridgeKey Zeros;
	TArray<TArray<uint8>> Messages = { FFlockBridgeMessages::WriteOffer(Zeros, Zeros, { Peer(TEXT("127.0.0.1:7777")) }), FFlockBridgeMessages::WriteNewConnection(7) };
	for (const uint8 Kind : { FFlockBridgeMessages::Probe, FFlockBridgeMessages::Answer, FFlockBridgeMessages::KeepAlive, FFlockBridgeMessages::BackToTheRelay })
	{
		Messages.Add(FFlockBridgeMessages::WriteBetween(Kind, Zeros, Zeros));
	}
	for (const TArray<uint8>& Message : Messages)
	{
		TestTrue(FString::Printf(TEXT("kind %d is the bridges' own"), Message[5]), FFlockBridgeMessages::IsBridgeMessage(Message.GetData(), Message.Num()));
	}
	// A later SDK's message, of a version this one does not read, is kept from the engine all the same.
	TArray<uint8> Later = Messages[0];
	Later[4] = FFlockBridgeMessages::Version + 1;
	TestTrue(TEXT("a later version's message is the bridges' own"), FFlockBridgeMessages::IsBridgeMessage(Later.GetData(), Later.Num()));
	TestEqual(TEXT("and has no kind this version reads"), static_cast<int32>(FFlockBridgeMessages::KindOf(Later.GetData(), Later.Num())), 0);
	const uint8 LettersOnly[] = { FFlockBridgeMessages::Marker, 'L', 'N', 'K' };
	TestTrue(TEXT("the marker and the letters alone"), FFlockBridgeMessages::IsBridgeMessage(LettersOnly, UE_ARRAY_COUNT(LettersOnly)));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectOtherKeysTest, "Messages.AMessageNamingOtherKeys_IsNotBetweenThisPair")
bool FFlockDirectOtherKeysTest::RunTest(const FString& Parameters)
{
	const FFlockBridgeKey Ours = Repeated(1);
	const FFlockBridgeKey Theirs = Repeated(2);
	const TArray<uint8> Probe = FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Probe, Ours, Theirs);
	TestTrue(TEXT("between this pair"), FFlockBridgeMessages::IsBetween(Probe.GetData(), Probe.Num(), Ours, Theirs));
	TestFalse(TEXT("The keys the other way round"), FFlockBridgeMessages::IsBetween(Probe.GetData(), Probe.Num(), Theirs, Ours));
	TestFalse(TEXT("From an end other than the one this link knows"), FFlockBridgeMessages::IsBetween(Probe.GetData(), Probe.Num(), Ours, Repeated(9)));
	TestFalse(TEXT("Cut short"), FFlockBridgeMessages::IsBetween(Probe.GetData(), Probe.Num() - 1, Ours, Theirs));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectNewConnectionWordTest, "Messages.WordOfANewConnection_ReadsBackAsWritten")
bool FFlockDirectNewConnectionWordTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Word = FFlockBridgeMessages::WriteNewConnection(0xDEADBEEF);
	uint32 Number = 0;
	TestTrue(TEXT("read"), FFlockBridgeMessages::TryReadNewConnection(Word.GetData(), Word.Num(), Number));
	TestEqual(TEXT("its number"), Number, 0xDEADBEEFu);
	TestFalse(TEXT("Cut short"), FFlockBridgeMessages::TryReadNewConnection(Word.GetData(), Word.Num() - 1, Number));
	const TArray<uint8> Offer = FFlockBridgeMessages::WriteOffer(Key(1), TOptional<FFlockBridgeKey>(), { Peer(TEXT("127.0.0.1:7777")) });
	TestFalse(TEXT("An offer is no word of a connection"), FFlockBridgeMessages::TryReadNewConnection(Offer.GetData(), Offer.Num(), Number));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectKeysTest, "Messages.EachKey_IsNewAndNeverZeros")
bool FFlockDirectKeysTest::RunTest(const FString& Parameters)
{
	// A key of zeros reads, in an offer, as no key held; two links with one key would answer each other's strangers.
	TSet<FString> Seen;
	const FFlockBridgeKey Zeros;
	for (int32 Index = 0; Index < 100; ++Index)
	{
		const FFlockBridgeKey Made = FFlockBridgeKey::NewRandom();
		TestTrue(TEXT("not zeros"), Made != Zeros);
		Seen.Add(FString::FromBlob(Made.Bytes, FFlockBridgeKey::Length));
	}
	TestEqual(TEXT("100 keys, each new"), Seen.Num(), 100);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectPublishedReadTest, "Messages.ARelayConnection_SaysItsBridgeReadsTheMessages_WithANumberAlone")
bool FFlockDirectPublishedReadTest::RunTest(const FString& Parameters)
{
	const auto Connection = [](const FString& Mode, const TSharedPtr<FJsonValue>& Version)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("mode"), Mode);
		Object->SetStringField(FlockRelayConnectionValues::RelayAddress, TEXT("203.0.113.50:49152"));
		if (Version.IsValid())
		{
			Object->SetField(FlockRelayConnectionValues::BridgeMessages, Version);
		}
		return Object;
	};
	TestTrue(TEXT("version 1"), FFlockRelayPeer::ReadsBridgeMessages(*Connection(TEXT("relay"), MakeShared<FJsonValueNumber>(1))));
	TestTrue(TEXT("a later version"), FFlockRelayPeer::ReadsBridgeMessages(*Connection(TEXT("relay"), MakeShared<FJsonValueNumber>(2))));
	TestFalse(TEXT("absent: a bridge that hands them to the engine"), FFlockRelayPeer::ReadsBridgeMessages(*Connection(TEXT("relay"), nullptr)));
	TestFalse(TEXT("0"), FFlockRelayPeer::ReadsBridgeMessages(*Connection(TEXT("relay"), MakeShared<FJsonValueNumber>(0))));
	TestFalse(TEXT("as text"), FFlockRelayPeer::ReadsBridgeMessages(*Connection(TEXT("relay"), MakeShared<FJsonValueString>(TEXT("1")))));
	TestFalse(TEXT("another mode"), FFlockRelayPeer::ReadsBridgeMessages(*Connection(TEXT("direct"), MakeShared<FJsonValueNumber>(1))));
	TestFalse(TEXT("the key in another letter case"), [&]()
	{
		const TSharedRef<FJsonObject> Object = Connection(TEXT("relay"), nullptr);
		Object->SetNumberField(TEXT("Bridge_Messages"), 1);
		return FFlockRelayPeer::ReadsBridgeMessages(*Object);
	}());
	return true;
}

// ---- two bridges ----

FLOCK_DIRECT_PATH_TEST(FFlockDirectMovesTest, "TwoBridgesThatReachEachOther_MoveTheEnginesPacketsToADirectPath")
bool FFlockDirectMovesTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	// A connection's first three packets carry word of it to the host through the relay; after them, nothing more goes through it.
	F.OneEachWay(*this, Pair, TEXT("direct"));
	F.OneEachWay(*this, Pair, TEXT("direct-again"));
	const int32 HostRelayed = F.RelayedFrom(Pair.HostRelay);
	const int32 PlayerRelayed = F.RelayedFrom(Pair.PlayerRelay);
	F.OneEachWay(*this, Pair, TEXT("direct-a-third-time"));
	F.OneEachWay(*this, Pair, TEXT("direct-a-fourth-time"));
	TestEqual(TEXT("The player's packets no longer go through the relay"), F.RelayedFrom(Pair.PlayerRelay), PlayerRelayed);
	TestEqual(TEXT("The host's packets no longer go through the relay"), F.RelayedFrom(Pair.HostRelay), HostRelayed);
	TestTrue(TEXT("Saying so in the log"), F.InfoLogged(TEXT("now go directly")));
	TestEqual(TEXT("The relay client counts what it sent on as the relay saw it"), static_cast<int32>(Pair.PlayerRelay->GetPacketsSent()), F.RelayedFrom(Pair.PlayerRelay));
	TestEqual(TEXT("and the host's"), static_cast<int32>(Pair.HostRelay->GetPacketsSent()), F.RelayedFrom(Pair.HostRelay));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectStunTest, "AnEndTheRelayServerDoesNotAnswer_OffersTheAddressFlocksStunServerSaw_AndStillGoesDirect")
bool FFlockDirectStunTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	F.Stun = MakeUnique<FFlockFakeStunServer>();
	if (!TestTrue(TEXT("precondition: a STUN server"), F.Stun->IsReady()))
	{
		return false;
	}
	F.Stun->PublicAddress = TEXT("203.0.113.7");
	F.StunServers.Add(FFlockRelayStunLookUp{ FFlockNameLookup::Start(TEXT("127.0.0.1"), FFlockNameLookup::FLookUp()), F.Stun->GetPort() });
	FPair Pair;
	// As Cloudflare's relay server answers most of a device's ports nothing: only the STUN server Flock lists says where it sees the link.
	if (!F.Paired(*this, Pair, FPairOptions(), [&F]() { F.Relay->bAnswersBindings = false; }))
	{
		return false;
	}
	F.Settle([&]() { return Pair.PlayersLink().IsValid() && Pair.PlayersLink()->GetTheirAddressesForTesting().Num() > 0; }, 5.0);
	const TArray<FFlockRelayPeer> Offered = Pair.PlayersLink().IsValid() ? Pair.PlayersLink()->GetTheirAddressesForTesting() : TArray<FFlockRelayPeer>();
	TestTrue(TEXT("The host offered the address the STUN server saw"), Offered.ContainsByPredicate([](const FFlockRelayPeer& Address) { return Address.Ip() == TEXT("203.0.113.7"); }));
	TestTrue(TEXT("The STUN server was asked"), F.Stun->Requests > 0);
	F.BothDirect(*this, Pair);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectDualStackTest, "TwoBridges_ReachEachOtherFromDualStackSockets_AndReadWhatTheyHearAsIpv4")
bool FFlockDirectDualStackTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FName DualStack = ProtocolThisMachineGivesARelaySocket();
	TestEqual(TEXT("The relay socket is dual-stack where the platform allows"), Pair.HostRelay->GetSocketProtocolForTesting(), DualStack);
	TestEqual(TEXT("So is each link's"), Pair.HostsLink()->GetProtocolForTesting(), DualStack);
	TestEqual(TEXT("and the player's"), Pair.PlayersLink()->GetProtocolForTesting(), DualStack);
	// The relay server heard on a dual-stack socket is still known as the server that said where it sees the link.
	const TOptional<FFlockRelayPeer> Public = Pair.PlayersLink()->GetPublicAddressForTesting();
	TestTrue(TEXT("The player's link learned where the relay server sees it, as IPv4"), Public.IsSet() && Public.GetValue().Ip() == TEXT("127.0.0.1"));
	const TArray<FFlockRelayPeer> Heard = Pair.PlayersLink()->GetTheirAddressesForTesting();
	TestTrue(TEXT("The host's addresses, offered and probed from, read as IPv4 on this machine's loopback"), Heard.Num() > 0
		&& !Heard.ContainsByPredicate([](const FFlockRelayPeer& Address) { return Address.Ip() != TEXT("127.0.0.1"); }));
	F.OneEachWay(*this, Pair, TEXT("direct"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectIpv4Test, "WithoutIpv6_TwoBridges_StillGoDirect_FromPlainIpv4Sockets")
bool FFlockDirectIpv4Test::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FFlockSockets::bDualStackRefusedForTesting = true;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FName Ipv4 = FNetworkProtocolTypes::IPv4;
	TestEqual(TEXT("A platform without IPv6 gets a plain IPv4 relay socket"), Pair.HostRelay->GetSocketProtocolForTesting(), Ipv4);
	TestEqual(TEXT("and plain IPv4 links"), Pair.HostsLink()->GetProtocolForTesting(), Ipv4);
	TestEqual(TEXT("both"), Pair.PlayersLink()->GetProtocolForTesting(), Ipv4);
	F.OneEachWay(*this, Pair, TEXT("direct"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectNotLetInTest, "AHost_OffersNothing_ToAPlayerItsCheckHasNotLetIn")
bool FFlockDirectNotLetInTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bLetIn = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	// The next reading of the session names the player's seat again, now that it has a place: still no link before it is let in.
	Pair.Host->SetPeersThatReadBridgeMessages({ Pair.PlayersAddress() });
	TestTrue(TEXT("the player gave up waiting for the host's offer"), F.Settle([&]() { return Pair.PlayersLink().IsValid() && Pair.PlayersLink()->HasStopped(); }, 6.0));
	TestFalse(TEXT("No link to a player the check never let in"), Pair.HostsLink().IsValid());
	TestFalse(TEXT("not direct"), Pair.PlayersLink().IsValid() && Pair.PlayersLink()->IsDirect());
	TestTrue(TEXT("The player says why it stays on the relay"), F.InfoLogged(TEXT("never offered its addresses")));
	F.OneEachWay(*this, Pair, TEXT("relayed"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectOfferedBeforeTest, "APlayerWhoOfferedBeforeItWasLetIn_GoesDirectOnceLetIn")
bool FFlockDirectOfferedBeforeTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bLetIn = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	// The player offers while the host's check runs, and none of it is taken.
	F.Seconds(0.6);
	TestFalse(TEXT("Precondition: nothing yet for a player not let in"), Pair.HostsLink().IsValid());
	Pair.Host->LetIn(Pair.PlayersAddress());
	F.BothDirect(*this, Pair);
	F.OneEachWay(*this, Pair, TEXT("after-let-in"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectPlayerOnTheRelayTest, "APlayerKeptOnTheRelay_OffersNothing_AndItsHostStaysOnTheRelay")
bool FFlockDirectPlayerOnTheRelayTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bPlayerGoesDirect = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	TestTrue(TEXT("the host gave up waiting for the player's offer"), F.Settle([&]() { return Pair.HostsLink().IsValid() && Pair.HostsLink()->HasStopped(); }, 6.0));
	TestFalse(TEXT("A player on the relay only (Relay Always) makes no link"), Pair.PlayersLink().IsValid());
	TestFalse(TEXT("not direct"), Pair.HostsLink()->IsDirect());
	F.OneEachWay(*this, Pair, TEXT("relayed"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectHostOnTheRelayTest, "AHostKeptOnTheRelay_OffersNothing_ToAPlayerItLetInThatReadsTheMessages")
bool FFlockDirectHostOnTheRelayTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bHostGoesDirect = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	// The session's reading again, as every 2 s: still nothing offered.
	Pair.Host->SetPeersThatReadBridgeMessages({ Pair.PlayersAddress() });
	TestTrue(TEXT("the player gave up waiting for the host's offer"), F.Settle([&]() { return Pair.PlayersLink().IsValid() && Pair.PlayersLink()->HasStopped(); }, 6.0));
	TestFalse(TEXT("A host on the relay only (Relay Always) makes no link"), Pair.HostsLink().IsValid());
	F.OneEachWay(*this, Pair, TEXT("relayed"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectNeverToTheEngineTest, "TheBridgesOwnMessages_NeverReachTheEngine_WhateverTheirVersion")
bool FFlockDirectNeverToTheEngineTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bLetIn = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	const FFlockBridgeKey Zeros;
	TArray<uint8> Offer = FFlockBridgeMessages::WriteOffer(Zeros, TOptional<FFlockBridgeKey>(), { Peer(TEXT("127.0.0.1:9")) });
	TArray<uint8> Probe = FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Probe, Zeros, Zeros);
	TArray<uint8> Later = FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::KeepAlive, Zeros, Zeros);
	Later[4] = FFlockBridgeMessages::Version + 1;
	const TArray<uint8> Word = FFlockBridgeMessages::WriteNewConnection(42);
	TestTrue(TEXT("Precondition: an offer sent to the player through the relay"), Pair.HostRelay->Send(Pair.PlayersAddress(), Offer.GetData(), Offer.Num()));
	TestTrue(TEXT("and a later version's message"), Pair.HostRelay->Send(Pair.PlayersAddress(), Later.GetData(), Later.Num()));
	TestTrue(TEXT("Precondition: a probe sent to the host through the relay"), Pair.PlayerRelay->Send(Pair.HostsAddress(), Probe.GetData(), Probe.Num()));
	TestTrue(TEXT("and a later version's"), Pair.PlayerRelay->Send(Pair.HostsAddress(), Later.GetData(), Later.Num()));
	TestTrue(TEXT("and word of a new connection"), Pair.PlayerRelay->Send(Pair.HostsAddress(), Word.GetData(), Word.Num()));
	F.Sends(Pair.HostRelay, Pair.PlayersAddress(), TEXT("control"));
	F.Sends(Pair.PlayerRelay, Pair.HostsAddress(), TEXT("control"));
	F.EngineHears(*Pair.PlayerEngine, TEXT("control"));
	F.EngineHears(*Pair.HostEngine, TEXT("control"));
	F.Seconds(0.3);
	Pair.PlayerEngine->ReadEverything();
	Pair.HostEngine->ReadEverything();
	TestEqual(TEXT("The player's engine heard only the plain packet"), Pair.PlayerEngine->Heard.Num(), 1);
	// "hello" and "control" alone.
	TestEqual(TEXT("The host's engine heard only the plain packets"), Pair.HostEngine->Heard.Num(), 2);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectStrangerTest, "AProbeOrAPacketFromAnyoneElse_OnTheDirectSocket_IsDropped")
bool FFlockDirectStrangerTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bLetIn = false;
	if (!F.Paired(*this, Pair, Options) || !TestTrue(TEXT("precondition: the player's link"), Pair.PlayersLink().IsValid()))
	{
		return false;
	}
	const int32 LinkPort = Pair.PlayersLink()->GetLocalPortForTesting();
	FEngineSocket Stranger;
	const FFlockBridgeKey Zeros;
	F.StrangerSends(Stranger, LinkPort, FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Probe, Zeros, Zeros), 5);
	F.StrangerSends(Stranger, LinkPort, FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Answer, Zeros, Zeros), 5);
	F.StrangerSends(Stranger, LinkPort, Bytes(TEXT("from-a-stranger")), 5);
	F.Seconds(0.5);
	TestFalse(TEXT("Probes and answers without the keys open nothing"), Pair.PlayersLink()->IsDirect());
	Pair.PlayerEngine->ReadEverything();
	TestEqual(TEXT("A stranger's packet never reaches the engine"), Pair.PlayerEngine->PortOf(TEXT("from-a-stranger")), 0);
	Stranger.ReadEverything();
	TestEqual(TEXT("Nothing was answered to the stranger"), Stranger.Heard.Num(), 0);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectStrangersProbeTest, "WhileTheEndsProbe_AStrangersProbeNamingOtherKeys_OpensNothing")
bool FFlockDirectStrangersProbeTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FFlockDirectLink::FTiming ProbingLonger = QuickLinks();
	ProbingLonger.ProbeForSeconds = 5.0;
	FFlockDirectLink::TimingForTesting = ProbingLonger;
	FPair Pair;
	FPairOptions Options;
	Options.bLetIn = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	Pair.PlayersLink()->bDropsAnswersForTesting = true;
	Pair.Host->LetIn(Pair.PlayersAddress());
	if (!TestTrue(TEXT("the host's link"), F.Settle([&]() { return Pair.HostsLink().IsValid(); }, 2.0)))
	{
		return false;
	}
	Pair.HostsLink()->bDropsAnswersForTesting = true;
	TestTrue(TEXT("the player knows the host's key and probes"), F.Settle([&]() { return Pair.PlayersLink()->IsProbingForTesting(); }, 3.0));
	const int32 LinkPort = Pair.PlayersLink()->GetLocalPortForTesting();
	FEngineSocket Stranger;
	const FFlockBridgeKey Other = Repeated(9);
	F.StrangerSends(Stranger, LinkPort, FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::Probe, Other, Other), 5);
	F.StrangerSends(Stranger, LinkPort, Bytes(TEXT("from-a-stranger")), 5);
	F.Seconds(0.3);
	TestTrue(TEXT("Precondition: still probing"), Pair.PlayersLink()->IsProbingForTesting());
	Stranger.ReadEverything();
	TestEqual(TEXT("Nothing was answered to the stranger"), Stranger.Heard.Num(), 0);
	Pair.PlayerEngine->ReadEverything();
	TestEqual(TEXT("A stranger's packet never reaches the engine"), Pair.PlayerEngine->PortOf(TEXT("from-a-stranger")), 0);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectKeptAliveTest, "ADirectPathTheEngineLeavesQuiet_StaysDirect_OnTheLinksKeepAlives")
bool FFlockDirectKeptAliveTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	// Twice the quiet time and more, with nothing from either engine.
	F.Seconds(2.0);
	TestTrue(TEXT("both still direct"), Pair.HostsLink()->IsDirect() && Pair.PlayersLink()->IsDirect());
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectStaleWordTest, "WordOfGoingBackNamingOtherKeys_LeavesTheDirectPathAsItIs")
bool FFlockDirectStaleWordTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	// As an earlier link to the same player, made before it connected again, would have sent it.
	const FFlockBridgeKey Other = Repeated(9);
	const TArray<uint8> Stale = FFlockBridgeMessages::WriteBetween(FFlockBridgeMessages::BackToTheRelay, Other, Other);
	TestTrue(TEXT("Precondition: sent to the host through the relay"), Pair.PlayerRelay->Send(Pair.HostsAddress(), Stale.GetData(), Stale.Num()));
	F.Seconds(0.3);
	TestTrue(TEXT("Still direct"), Pair.HostsLink()->IsDirect());
	const int32 HostRelayed = F.RelayedFrom(Pair.HostRelay);
	F.OneEachWay(*this, Pair, TEXT("still-direct"));
	TestEqual(TEXT("nothing more through the relay"), F.RelayedFrom(Pair.HostRelay), HostRelayed);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectGoesQuietTest, "ADirectPathThatGoesQuiet_GoesBackToTheRelay_AndTheOtherEndFollows")
bool FFlockDirectGoesQuietTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	Pair.HostsLink()->bHearsNothingForTesting = true;
	if (!TestTrue(TEXT("the end that heard nothing went back to the relay"), F.Settle([&]() { return Pair.HostsLink()->HasStopped(); }, 4.0)))
	{
		return false;
	}
	const double HostWentBack = FPlatformTime::Seconds();
	TestTrue(TEXT("both went back to the relay"), F.Settle([&]() { return Pair.PlayersLink()->HasStopped(); }, 4.0));
	// The player heard the host's keep-alives until the host went back, so its own quiet would take most of QuietForSeconds more.
	const double Followed = FPlatformTime::Seconds() - HostWentBack;
	const double OwnQuiet = FFlockDirectLink::TimingForTesting.GetValue().QuietForSeconds;
	TestTrue(FString::Printf(TEXT("The other end followed on the word, before its own %.1f s of quiet (%.2f s)"), OwnQuiet, Followed), Followed < OwnQuiet / 2.0);
	TestTrue(TEXT("The end that heard nothing says so"), F.InfoLogged(TEXT("went quiet")));
	TestTrue(TEXT("The other end follows when told"), F.InfoLogged(TEXT("went back to Flock's relay")));
	const int32 PlayerRelayed = F.RelayedFrom(Pair.PlayerRelay);
	F.OneEachWay(*this, Pair, TEXT("back-on-the-relay"));
	TestTrue(TEXT("The player's packets go through the relay again"), F.RelayedFrom(Pair.PlayerRelay) > PlayerRelayed);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectOneWayTest, "AnswersThatNeverComeBack_LeaveThePairOnTheRelay")
bool FFlockDirectOneWayTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bLetIn = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	Pair.PlayersLink()->bDropsAnswersForTesting = true;
	Pair.Host->LetIn(Pair.PlayersAddress());
	if (!TestTrue(TEXT("the host's link"), F.Settle([&]() { return Pair.HostsLink().IsValid(); }, 2.0)))
	{
		return false;
	}
	Pair.HostsLink()->bDropsAnswersForTesting = true;
	TestTrue(TEXT("both gave up probing"), F.Settle([&]() { return Pair.HostsLink()->HasStopped() && Pair.PlayersLink()->HasStopped(); }, 6.0));
	TestFalse(TEXT("A probe heard one way only is not a direct path"), Pair.HostsLink()->IsDirect() || Pair.PlayersLink()->IsDirect());
	TestTrue(TEXT("said why"), F.InfoLogged(TEXT("no probe was answered")));
	// The player's hello and a few offers each: none once each end holds the other's key, though they probed for a while.
	TestTrue(TEXT("The player's offers stopped"), F.RelayedFrom(Pair.PlayerRelay) < 8);
	TestTrue(TEXT("The host's offers stopped"), F.RelayedFrom(Pair.HostRelay) < 8);
	F.OneEachWay(*this, Pair, TEXT("relayed"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectRoutersTest, "EndsBehindRoutersThatDropTheUnasked_StillOpenTheDirectPath_BecauseBothProbe")
bool FFlockDirectRoutersTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bLetIn = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	Pair.PlayersLink()->bFiltersLikeARouterForTesting = true;
	Pair.Host->LetIn(Pair.PlayersAddress());
	if (!TestTrue(TEXT("the host's link"), F.Settle([&]() { return Pair.HostsLink().IsValid(); }, 2.0)))
	{
		return false;
	}
	Pair.HostsLink()->bFiltersLikeARouterForTesting = true;
	F.BothDirect(*this, Pair);
	F.OneEachWay(*this, Pair, TEXT("through-routers"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectWhereProbesCameFromTest, "AnEndReachedOnlyAtTheAddressItsProbesCameFrom_StillGoesDirect")
bool FFlockDirectWhereProbesCameFromTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	// The player's router gives each destination a port of its own, so the address it offers reaches nothing.
	FFlockDirectLink::OfferInsteadForTesting({ Peer(TEXT("127.0.0.2:9")) });
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	F.OneEachWay(*this, Pair, TEXT("where-the-probes-came-from"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectNewBridgeTest, "APlayerLetInAgain_ThroughANewBridge_GetsANewLink_AndGoesDirect")
bool FFlockDirectNewBridgeTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FFlockDirectLink::FPtr First = Pair.HostsLink();
	// The player's game travels again on the same relay address: a new bridge, so a new link with a new key.
	Pair.Player->Close();
	FFlockRelayBridge::FToTheHostOptions Bridging;
	Bridging.bHostReadsBridgeMessages = true;
	Bridging.bGoesDirect = true;
	Pair.Player = F.Kept(*this, FFlockRelayBridge::ToTheHost(Pair.PlayerRelay.ToSharedRef(), Pair.HostsAddress(), F.Log, Bridging));
	Pair.PlayerEngine = MakeUnique<FEngineSocket>();
	Pair.PlayerEngine->SendTo(Pair.Player->GetLocalPort(), TEXT("hello-again"));
	if (!TestTrue(TEXT("the host's engine heard the new connection"), F.EngineHears(*Pair.HostEngine, TEXT("hello-again"))))
	{
		return false;
	}
	// The new connection reaches the engine from a loopback port of its own (it said it is new), and is let in there.
	Pair.PlayersPortAtTheHost = Pair.HostEngine->PortOf(TEXT("hello-again"));
	// Its new link offers while the host's check runs: no new link for it until the check lets it in.
	if (!TestTrue(TEXT("the player's new link learned its address, so it offers"), F.Settle([&]()
		{
			const FFlockDirectLink::FPtr Players = Pair.PlayersLink();
			return Players.IsValid() && Players->GetPublicAddressForTesting().IsSet();
		}, 3.0)))
	{
		return false;
	}
	F.Seconds(0.6);
	TestTrue(TEXT("no new link before the new connection is let in"), Pair.HostsLink() == First);
	Pair.Host->LetIn(Pair.PlayersAddress());
	TestTrue(TEXT("The link to the player's earlier bridge is let go"), F.Settle([&]() { return First->HasStopped(); }, 3.0));
	TestTrue(TEXT("a link of its own"), Pair.HostsLink() != First);
	F.BothDirect(*this, Pair);
	F.OneEachWay(*this, Pair, TEXT("after-the-restart"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectClosingTest, "Closing_ClosesEveryLinksSocket")
bool FFlockDirectClosingTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FFlockDirectLink::FPtr Hosts = Pair.HostsLink();
	const FFlockDirectLink::FPtr Players = Pair.PlayersLink();
	const int32 HostsPort = Hosts->GetLocalPortForTesting();
	Pair.Host->Close();
	Pair.Player->Close();
	TestTrue(TEXT("both links stopped"), Hosts->HasStopped() && Players->HasStopped());
	TestTrue(TEXT("their threads ended"), F.Settle([&]() { return Hosts->HasEnded() && Players->HasEnded(); }, 2.0));
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Again = Sockets->CreateSocket(NAME_DGram, TEXT("Flock test port check"), FNetworkProtocolTypes::IPv4);
	const TSharedRef<FInternetAddr> Anywhere = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	Anywhere->SetAnyAddress();
	Anywhere->SetPort(HostsPort);
	TestTrue(TEXT("The host's link let its port go"), Again != nullptr && Again->Bind(*Anywhere));
	FFlockSockets::Close(Again);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectForgetTest, "AHostThatForgetsAPlayer_ClosesItsLink")
bool FFlockDirectForgetTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FFlockDirectLink::FPtr Link = Pair.HostsLink();
	Pair.Host->Forget(Pair.PlayersAddress());
	TestTrue(TEXT("stopped"), Link->HasStopped());
	TestFalse(TEXT("The forgotten player has no link any more"), Pair.HostsLink().IsValid());
	return true;
}

// ---- Unreal: a bridge that outlives its engine's connections ----

FLOCK_DIRECT_PATH_TEST(FFlockDirectConnectsAgainTest, "APlayersGameConnectingAgain_GetsANewLink_AndTheHostMakesOneForIt")
bool FFlockDirectConnectsAgainTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FFlockDirectLink::FPtr PlayersFirst = Pair.PlayersLink();
	const FFlockDirectLink::FPtr HostsFirst = Pair.HostsLink();
	// The net driver connects again from a new port (a host's level change sends it back), through the same bridge, as Travel To
	// Session sees it.
	Pair.Player->GameConnectsAgain();
	Pair.PlayerEngine = MakeUnique<FEngineSocket>();
	Pair.PlayerEngine->SendTo(Pair.Player->GetLocalPort(), TEXT("hello-again"));
	if (!TestTrue(TEXT("the host's engine heard the new connection"), F.EngineHears(*Pair.HostEngine, TEXT("hello-again"))))
	{
		return false;
	}
	Pair.PlayersPortAtTheHost = Pair.HostEngine->PortOf(TEXT("hello-again"));
	Pair.Host->LetIn(Pair.PlayersAddress());
	TestTrue(TEXT("The player's link for its earlier connection is let go"), F.Settle([&]() { return PlayersFirst->HasStopped(); }, 3.0));
	TestTrue(TEXT("and the host's, once the new link's offer named a new key"), F.Settle([&]() { return HostsFirst->HasStopped(); }, 3.0));
	F.BothDirect(*this, Pair);
	F.OneEachWay(*this, Pair, TEXT("after-connecting-again"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectNoLongerLetInTest, "APlayerNoLongerLetIn_KeepsItsPlaceAndItsLink")
bool FFlockDirectNoLongerLetInTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FFlockDirectLink::FPtr Link = Pair.HostsLink();
	Pair.Host->NoLongerLetIn(Pair.PlayersAddress());
	TestTrue(TEXT("the same link, still direct"), Pair.HostsLink() == Link && Link->IsDirect());
	TestEqual(TEXT("the same place"), Pair.Host->LoopbackPortOf(Pair.PlayersAddress()), Pair.PlayersPortAtTheHost);
	F.OneEachWay(*this, Pair, TEXT("waiting-to-be-let-in"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectNewPlaceTest, "ANewConnectionThroughARelayAddressAnEarlierOneCameThrough_ReachesTheEngineFromANewPort")
bool FFlockDirectNewPlaceTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	// Relay Always on both: the word of a new connection does not need a direct path.
	FPairOptions Options;
	Options.bHostGoesDirect = false;
	Options.bPlayerGoesDirect = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	F.Seconds(0.2);
	TestEqual(TEXT("The first connection's word moved nothing: the place was made for it"), Pair.Host->LoopbackPortOf(Pair.PlayersAddress()), Pair.PlayersPortAtTheHost);
	const int32 EarlierPort = Pair.PlayersPortAtTheHost;
	// The player's game connects again (the engine's new socket) while its earlier connection is still on the host: the engine would
	// hand the new handshake to the earlier connection, from the same address, until that one timed out (a minute, measured).
	FEngineSocket Again;
	Pair.Player->GameConnectsAgain();
	Again.SendTo(Pair.Player->GetLocalPort(), TEXT("handshake-again"));
	if (!TestTrue(TEXT("the host's engine heard the new connection"), F.EngineHears(*Pair.HostEngine, TEXT("handshake-again"))))
	{
		return false;
	}
	const int32 NewPort = Pair.HostEngine->PortOf(TEXT("handshake-again"));
	TestNotEqual(TEXT("From a loopback port of its own"), NewPort, EarlierPort);
	TestEqual(TEXT("which is the player's place now"), Pair.Host->LoopbackPortOf(Pair.PlayersAddress()), NewPort);
	TestFalse(TEXT("The earlier port is given back"), Pair.Host->GetPeerPortsForTesting().Contains(EarlierPort));
	TestFalse(TEXT("The new connection waits to be let in: no channel for it until it is"), Pair.HostRelay->KeepsChannelToForTesting(Pair.PlayersAddress()));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectRemadeSocketTest, "AnEngineThatRemadeItsSocket_KeepsItsPlaceAndItsLink")
bool FFlockDirectRemadeSocketTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FFlockDirectLink::FPtr Players = Pair.PlayersLink();
	const FFlockDirectLink::FPtr Hosts = Pair.HostsLink();
	// The net driver remakes its socket for the same connection (net.RecreateSocketTimeoutThreshold): a new port, no new connection.
	Pair.PlayerEngine = MakeUnique<FEngineSocket>();
	F.OneEachWay(*this, Pair, TEXT("from-a-remade-socket"));
	F.Seconds(0.5);
	TestEqual(TEXT("the same place: the engine's connection goes on"), Pair.Host->LoopbackPortOf(Pair.PlayersAddress()), Pair.PlayersPortAtTheHost);
	TestTrue(TEXT("the same links, still direct"), Pair.PlayersLink() == Players && Pair.HostsLink() == Hosts && Players->IsDirect() && Hosts->IsDirect());
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectLetGoLinesTest, "ALinkLetGo_StillHasItsLastLineSaid")
bool FFlockDirectLetGoLinesTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	// The host's link goes quiet, and the player is forgotten before the bridge's next frame says so.
	const FFlockDirectLink::FPtr Link = Pair.HostsLink();
	Link->bHearsNothingForTesting = true;
	F.Until([&]() { return Link->HasStopped(); }, 3.0);
	TestFalse(TEXT("precondition: not said yet"), F.InfoLogged(TEXT("went quiet")));
	Pair.Host->Forget(Pair.PlayersAddress());
	Pair.Host->Tick();
	TestTrue(TEXT("said at the next frame all the same"), F.InfoLogged(TEXT("went quiet")));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectStrangersWordsTest, "AStrangersWords_NeverHoldAPlayersOutForLong")
bool FFlockDirectStrangersWordsTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Stranger = F.Opened(*this, TEXT("player-stranger"));
	const TSharedRef<FOpenedTo> StrangerOpen = F.OpenTo(Stranger, Host->GetAddress());
	F.Until([&]() { return StrangerOpen->bAnswered; });
	FEngineSocket Engine;
	FFlockRelayBridge::MostPlayersForTesting = 1;
	FFlockRelayBridge::NotLetInWithinForTesting = 0.3;
	const FFlockRelayBridge::FPtr Bridge = F.Kept(*this, FFlockRelayBridge::ForTheHost(Host.ToSharedRef(), Engine.Port, F.Log, false));
	if (!Stranger.IsValid() || !Bridge.IsValid())
	{
		return false;
	}
	// A stranger's word holds the one place words are kept for, and ages out.
	const TArray<uint8> StrangersWord = FFlockBridgeMessages::WriteNewConnection(5);
	Stranger->Send(Host->GetAddress(), StrangersWord.GetData(), StrangersWord.Num());
	F.Seconds(0.5);
	// The player's word, then its first packet: its place knows its connection.
	const TArray<uint8> First = FFlockBridgeMessages::WriteNewConnection(7);
	Player->Send(Host->GetAddress(), First.GetData(), First.Num());
	F.Seconds(0.1);
	F.Sends(Player, Host->GetAddress(), TEXT("first"));
	F.EngineHears(Engine, TEXT("first"));
	const int32 Place = Engine.PortOf(TEXT("first"));
	// Its game connects again: a place of its own for the new connection.
	const TArray<uint8> Second = FFlockBridgeMessages::WriteNewConnection(8);
	Player->Send(Host->GetAddress(), Second.GetData(), Second.Num());
	F.Seconds(0.1);
	F.Sends(Player, Host->GetAddress(), TEXT("second"));
	F.EngineHears(Engine, TEXT("second"));
	TestTrue(TEXT("precondition: the first packet made a place"), Place > 0);
	TestNotEqual(TEXT("the new connection reaches the engine from a new place"), Engine.PortOf(TEXT("second")), Place);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectSameWordTest, "WordOfTheSameNewConnectionAgain_MovesItOnce")
bool FFlockDirectSameWordTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bHostGoesDirect = false;
	Options.bPlayerGoesDirect = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	const TArray<uint8> Word = FFlockBridgeMessages::WriteNewConnection(123456);
	Pair.PlayerRelay->Send(Pair.HostsAddress(), Word.GetData(), Word.Num());
	F.Settle([&]() { return Pair.Host->LoopbackPortOf(Pair.PlayersAddress()) != Pair.PlayersPortAtTheHost; }, 2.0);
	const int32 Moved = Pair.Host->LoopbackPortOf(Pair.PlayersAddress());
	TestNotEqual(TEXT("A connection another one came through before is moved"), Moved, Pair.PlayersPortAtTheHost);
	Pair.PlayerRelay->Send(Pair.HostsAddress(), Word.GetData(), Word.Num());
	Pair.PlayerRelay->Send(Pair.HostsAddress(), Word.GetData(), Word.Num());
	F.Seconds(0.3);
	TestEqual(TEXT("Word of the same connection again moves it no more"), Pair.Host->LoopbackPortOf(Pair.PlayersAddress()), Moved);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectQuietToOldHostsTest, "APlayersBridge_SendsNothingOfItsOwn_ToAHostThatDoesNotReadTheBridgesMessages")
bool FFlockDirectQuietToOldHostsTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	// The host's bridge hands every packet to its engine (one from before this version): its relay address is read here, as it.
	FFlockRelayBridge::FToTheHostOptions Bridging;
	Bridging.bHostReadsBridgeMessages = false;
	Bridging.bGoesDirect = true;
	const FFlockRelayBridge::FPtr Bridge = F.Kept(*this, FFlockRelayBridge::ToTheHost(Player.ToSharedRef(), Host->GetAddress(), F.Log, Bridging));
	if (!Bridge.IsValid())
	{
		return false;
	}
	TestFalse(TEXT("No link: a direct path is opened with messages that host would hand its engine"), Bridge->GetDirectLinkForTesting(Host->GetAddress()).IsValid());
	FEngineSocket First;
	FEngineSocket Second;
	First.SendTo(Bridge->GetLocalPort(), TEXT("first-connection"));
	F.Seconds(0.3);
	Second.SendTo(Bridge->GetLocalPort(), TEXT("second-connection"));
	F.Seconds(1.0);
	const TArray<TArray<uint8>> Heard = FDirectFixture::Received(Host);
	TestEqual(TEXT("The host's relay address got the engine's two packets"), Heard.Num(), 2);
	TestFalse(TEXT("and nothing of the bridge's own"), Heard.ContainsByPredicate([](const TArray<uint8>& Packet) { return FFlockBridgeMessages::IsBridgeMessage(Packet.GetData(), Packet.Num()); }));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectSeatSaysTest, "AHostOffersNoDirectPath_ToAPlayerWhoseSeatDoesNotSayItReadsTheMessages")
bool FFlockDirectSeatSaysTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	FPairOptions Options;
	Options.bPlayerReadsBridgeMessages = false;
	if (!F.Paired(*this, Pair, Options))
	{
		return false;
	}
	F.Seconds(0.6);
	TestFalse(TEXT("No link to a let-in player whose seat does not say so: its bridge would hand them to its engine"), Pair.HostsLink().IsValid());
	// The next reading of the session shows its seat saying so.
	Pair.Host->SetPeersThatReadBridgeMessages({ Pair.PlayersAddress() });
	F.BothDirect(*this, Pair);
	F.OneEachWay(*this, Pair, TEXT("once-its-seat-said-so"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectHostToAlwaysTest, "AHostThatTravelsAgainWithRelayAlways_ClosesItsLinks_AndThePacketsGoThroughTheRelay")
bool FFlockDirectHostToAlwaysTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair))
	{
		return false;
	}
	const FFlockDirectLink::FPtr Link = Pair.HostsLink();
	Pair.Host->SetGoesDirect(false);
	TestTrue(TEXT("its link stopped"), Link->HasStopped());
	TestFalse(TEXT("and is gone"), Pair.HostsLink().IsValid());
	TestTrue(TEXT("the player's goes back to the relay once the path is quiet"), F.Settle([&]() { return Pair.PlayersLink()->HasStopped(); }, 3.0));
	const int32 HostRelayed = F.RelayedFrom(Pair.HostRelay);
	F.OneEachWay(*this, Pair, TEXT("relay-always"));
	TestTrue(TEXT("The host's packets go through the relay"), F.RelayedFrom(Pair.HostRelay) > HostRelayed);
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectGameTimingTest, "AtTheTimingAGameUses_TwoBridgesGoDirect_AndBackWhenTheDirectPathGoesQuiet")
bool FFlockDirectGameTimingTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FFlockDirectLink::TimingForTesting.Reset();
	FPair Pair;
	if (!F.Paired(*this, Pair) || !F.BothDirect(*this, Pair, 8.0))
	{
		return false;
	}
	F.OneEachWay(*this, Pair, TEXT("direct"));
	const double QuietFrom = FPlatformTime::Seconds();
	Pair.HostsLink()->bHearsNothingForTesting = true;
	TestTrue(TEXT("both went back to the relay"), F.Settle([&]() { return Pair.HostsLink()->HasStopped() && Pair.PlayersLink()->HasStopped(); }, 7.0));
	const double Took = FPlatformTime::Seconds() - QuietFrom;
	TestTrue(FString::Printf(TEXT("after 4 s without a word, not sooner (%.2f s)"), Took), Took >= 3.9 && Took < 6.0);
	F.OneEachWay(*this, Pair, TEXT("back-on-the-relay"));
	return true;
}

FLOCK_DIRECT_PATH_TEST(FFlockDirectLinesTest, "ALinksLines_AreSaidByTheBridgeOnTheGameThread_AtInfo")
bool FFlockDirectLinesTest::RunTest(const FString& Parameters)
{
	FDirectFixture F;
	FPair Pair;
	if (!F.Paired(*this, Pair))
	{
		return false;
	}
	// The relays run, the bridges do not: what the links say waits for the game thread.
	F.Until([&]()
	{
		const FFlockDirectLink::FPtr Hosts = Pair.HostsLink();
		const FFlockDirectLink::FPtr Players = Pair.PlayersLink();
		return Hosts.IsValid() && Hosts->IsDirect() && Players.IsValid() && Players->IsDirect();
	}, 5.0);
	TestFalse(TEXT("nothing said before the bridges' frame"), F.InfoLogged(TEXT("now go directly")));
	Pair.Host->Tick();
	TestTrue(TEXT("said at Info once the host's bridge runs"), F.InfoLogged(TEXT("Packets to the player on relay address")));
	Pair.Player->Tick();
	TestTrue(TEXT("and the player's"), F.InfoLogged(TEXT("Packets to the host now go directly")));
	TestEqual(TEXT("nothing at Warning or Error"), F.Log->Warnings.Num() + F.Log->Errors.Num(), 0);
	return true;
}

#endif
