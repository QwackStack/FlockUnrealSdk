// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Multiplayer/FlockNameLookup.h"
#include "Multiplayer/FlockSockets.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Multiplayer/Relay/FlockRelayWays.h"
#include "Tests/Support/FlockFakeRelayServer.h"
#include "Tests/Support/FlockRelayFixture.h"
#include "Tests/Support/FlockTestSpelling.h"

// The relay reached at its server's IPv6 address, for a network with IPv6 alone, against the fake relay listening on both
// loopbacks (skipped on a machine with no IPv6 loopback, as some build containers): the relay address stays IPv4, so peers do;
// IPv6 is tried only once IPv4 does not open, and UDP over IPv6 before any stream; a platform without IPv6 refuses at once.

#define FLOCK_RELAY_IPV6_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Relay.Ipv6." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockRelayIpv6TestHelpers
{
	using namespace FlockRelayTestHelpers;

	FFlockRelayTiming LookingUp(FFlockNameLookup::FLookUp LookUp)
	{
		FFlockRelayTiming Timing = Quick();
		Timing.NameLookUpForTesting = MoveTemp(LookUp);
		return Timing;
	}

	/** False, with the reason said, on a machine where the fake relay has no IPv6 loopback to listen on. */
	bool HasIpv6(FAutomationTestBase& Test, const FFlockFakeRelayServer& Relay)
	{
		if (!Relay.HasIpv6())
		{
			Test.AddInfo(TEXT("Skipped: this machine has no IPv6 loopback for the fake relay to listen on."));
			return false;
		}
		return Test.TestTrue(TEXT("precondition: the fake relay takes TCP and TLS"), Relay.IsReady());
	}

	TSharedPtr<FFlockRelayConnection> Raced(FAutomationTestBase& Test, FRelayFixture& F, const TArray<FFlockRelayLogin>& Ways, const FFlockRelayTiming& Timing,
		double& OutSeconds)
	{
		const double Started = FPlatformTime::Seconds();
		const TSharedRef<FOpening> Opening = F.Race(Ways, Timing);
		F.Until([&]() { return Opening->bAnswered; }, 15.0);
		OutSeconds = FPlatformTime::Seconds() - Started;
		Test.TestTrue(FString::Printf(TEXT("the relay opened (%s)"), *Opening->Result.Failure.Error.Message), Opening->Result.bSuccess);
		return Opening->Result.bSuccess ? Opening->Result.Value : nullptr;
	}
}

using namespace FlockRelayIpv6TestHelpers;

FLOCK_RELAY_IPV6_TEST(FFlockRelayIpv6AloneTest, "OnIpv6AloneTheRelayOpensOverUdpOnIpv6WithAnIpv4RelayAddressAndCarriesPackets")
bool FFlockRelayIpv6AloneTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!HasIpv6(*this, *F.Relay))
	{
		return true;
	}
	F.Relay->bIgnoresIpv4 = true;
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Host = Raced(*this, F, { F.Login(RelayPlayerA, TEXT("relay.test")) }, LookingUp(EveryNameOnIpv6Loopback()), Took);
	const TSharedPtr<FFlockRelayConnection> Player = Raced(*this, F, { F.Login(RelayPlayerB, TEXT("relay.test")) }, LookingUp(EveryNameOnIpv6Loopback()), Took);
	if (!Host.IsValid() || !Player.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("over UDP on IPv6"), Host->GetWay(), FString::Printf(TEXT("relay.test:%d over UDP on IPv6"), F.Relay->GetPort()));
	TestTrue(TEXT("the relay address is IPv4, as the relay gives one to a client on IPv6"), Player->GetAddress().Ip().Equals(FFlockFakeRelayServer::RelayIp, ESearchCase::CaseSensitive));
	TestEqual(TEXT("no IPv4 server a direct link would ask where it sees it"), Host->GetServerEndpoint().Address, 0u);
	TestFalse(TEXT("every reservation came over IPv6"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).ContainsByPredicate([](const FFlockFakeRelayServer::FSeen& Seen)
	{
		return !Seen.bOverIpv6;
	}));
	TestTrue(TEXT("said why the relay is reached over IPv6"), F.Log->Infos.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("on IPv6")) && Line.Contains(TEXT("IPv6 alone")); }));
	const TSharedRef<FOpenedTo> HostOpen = F.OpenTo(Host, Host->GetAddress());
	const TSharedRef<FOpenedTo> PlayerOpen = F.OpenTo(Player, Host->GetAddress());
	F.Until([&]() { return HostOpen->bAnswered && PlayerOpen->bAnswered; });
	if (!TestTrue(TEXT("precondition: both opened to the host's relay address"), HostOpen->Result.bSuccess && PlayerOpen->Result.bSuccess))
	{
		return false;
	}
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("player to host on IPv6"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("host to player on IPv6"));
	Player->KeepChannelTo(Host->GetAddress());
	Host->KeepChannelTo(Player->GetAddress());
	F.Until([&]() { return Player->ChannelNumberForTesting(Host->GetAddress()) != 0 && Host->ChannelNumberForTesting(Player->GetAddress()) != 0
		&& F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num() >= 2; }, 5.0);
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("on a channel, 19 b."));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("back on a channel 1"));
	return true;
}

FLOCK_RELAY_IPV6_TEST(FFlockRelayIpv6UdpBlockedTest, "OnIpv6AloneWithUdpBlockedTheRelayOpensOverTcpOnIpv6")
bool FFlockRelayIpv6UdpBlockedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!HasIpv6(*this, *F.Relay))
	{
		return true;
	}
	F.Relay->bIgnoresIpv4 = true;
	F.Relay->bIgnoresUdp = true;
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = Raced(*this, F, { F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) },
		LookingUp(EveryNameOnIpv6Loopback()), Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("over TCP on IPv6"), Relay->GetWay(), FString::Printf(TEXT("relay.test:%d over TCP on IPv6"), F.Relay->GetTcpPort()));
	// The IPv4 ways end at once (the name gives no IPv4 address): UDP on IPv6 starts at once, TCP over IPv4 at 2 s fails at once, and
	// TCP on IPv6 starts then, though UDP on IPv6 is still waiting; not 2 s later.
	TestTrue(FString::Printf(TEXT("UDP on IPv6 had its 2 s, then TCP on IPv6 opened as soon as TCP over IPv4 failed (%.1f s)"), Took), Took > 1.8 && Took < 3.4);
	return true;
}

FLOCK_RELAY_IPV6_TEST(FFlockRelayIpv6Ipv4UnansweredTest, "AnIpv4AddressNothingAnswersIsGivenUpForTheServersIpv6Address")
bool FFlockRelayIpv6Ipv4UnansweredTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!HasIpv6(*this, *F.Relay))
	{
		return true;
	}
	// The name gives both, but nothing reaches the IPv4 one, as on a network with IPv6 alone that still hands out A records.
	F.Relay->bIgnoresIpv4 = true;
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = Raced(*this, F, { F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) },
		LookingUp(EveryNameOnLoopback(/*bWithIpv6*/ true)), Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("over UDP on IPv6"), Relay->GetWay(), FString::Printf(TEXT("relay.test:%d over UDP on IPv6"), F.Relay->GetPort()));
	TestTrue(FString::Printf(TEXT("opened on IPv6 once UDP over IPv4 had its 2 s (%.1f s)"), Took), Took > 1.8 && Took < 4.5);
	F.RealSeconds(1.0);
	TestEqual(TEXT("one reservation, the one kept"), F.Relay->Reservations(), 1);
	return true;
}

FLOCK_RELAY_IPV6_TEST(FFlockRelayIpv6HotspotTest, "ANetworkThatDropsUdpOverIpv4AloneStaysOnUdpOverIpv6")
bool FFlockRelayIpv6HotspotTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!HasIpv6(*this, *F.Relay))
	{
		return true;
	}
	// As the phone hotspot measured on 2026-10-25: UDP over IPv4 unanswered, TCP over IPv4 and everything over IPv6 working.
	F.Relay->bIgnoresUdpOverIpv4 = true;
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = Raced(*this, F, { F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) },
		LookingUp(EveryNameOnLoopback(/*bWithIpv6*/ true)), Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("UDP over IPv6, not a stream over IPv4: a stream holds packets back for a lost one"), Relay->GetWay(),
		FString::Printf(TEXT("relay.test:%d over UDP on IPv6"), F.Relay->GetPort()));
	F.RealSeconds(1.0);
	TestEqual(TEXT("no TCP connection was made"), F.Relay->StreamsAccepted(), 0);
	return true;
}

FLOCK_RELAY_IPV6_TEST(FFlockRelayIpv6DualStackTest, "ADualStackNetworkNeverTriesIpv6WhileIpv4Opens")
bool FFlockRelayIpv6DualStackTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!HasIpv6(*this, *F.Relay))
	{
		return true;
	}
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = Raced(*this, F, { F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) },
		LookingUp(EveryNameOnLoopback(/*bWithIpv6*/ true)), Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("over UDP on IPv4"), Relay->GetWay(), FString::Printf(TEXT("relay.test:%d over UDP"), F.Relay->GetPort()));
	F.RealSeconds(2.5);
	TestFalse(TEXT("nothing went over IPv6 while IPv4 worked"), F.Relay->AllSeen().ContainsByPredicate([](const FFlockFakeRelayServer::FSeen& Seen) { return Seen.bOverIpv6; }));
	TestEqual(TEXT("nor over TCP"), F.Relay->StreamsAccepted(), 0);
	TestFalse(TEXT("nothing to say when the relay opens as it always did"), F.Log->Infos.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("relay is reached")); }));
	return true;
}

FLOCK_RELAY_IPV6_TEST(FFlockRelayIpv6NotOnThisPlatformTest, "APlatformWithoutIpv6RefusesAnIpv6WayAtOnce")
bool FFlockRelayIpv6NotOnThisPlatformTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!TestTrue(TEXT("precondition: the fake relay takes TCP"), F.Relay->IsReady()))
	{
		return false;
	}
	// As on Android, whose engine sockets hold no IPv6 address; IPv4 unanswered, so the IPv6 ways are tried.
	FFlockSockets::bDualStackRefusedForTesting = true;
	F.Relay->bIgnoresIpv4 = true;
	FFlockRelayTiming Timing = LookingUp(EveryNameOnLoopback(/*bWithIpv6*/ true));
	Timing.SocketRoundWaitSeconds = 0.2;
	Timing.AnswerWaitSeconds = 1.0;
	const double Started = FPlatformTime::Seconds();
	const TSharedRef<FOpening> Opening = F.Race({ F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) },
		Timing);
	F.Until([&]() { return Opening->bAnswered; }, 10.0);
	const double Took = FPlatformTime::Seconds() - Started;
	FFlockSockets::bDualStackRefusedForTesting = false;
	const FString Message = Opening->Result.Failure.Error.Message;
	TestTrue(FString::Printf(TEXT("the UDP way over IPv6 was refused, and says why (%s)"), *Message),
		Message.Contains(TEXT("over UDP on IPv6 could not be tried: this device's sockets reach no IPv6 address here")));
	TestTrue(FString::Printf(TEXT("and the TCP way over IPv6 (%s)"), *Message),
		Message.Contains(TEXT("over TCP on IPv6 could not be tried: this device's sockets reach no IPv6 address here")));
	TestTrue(FString::Printf(TEXT("each at once, once the IPv4 ways had failed: no wait of their own (%.1f s)"), Took), Took < 3.0);
	TestFalse(TEXT("nothing went over IPv6"), F.Relay->AllSeen().ContainsByPredicate([](const FFlockFakeRelayServer::FSeen& Seen) { return Seen.bOverIpv6; }));
	return true;
}

FLOCK_RELAY_IPV6_TEST(FFlockRelayIpv6RealLookUpTest, "TheSystemsLookUpFindsANamesAddressInTheFamilyAsked")
bool FFlockRelayIpv6RealLookUpTest::RunTest(const FString& Parameters)
{
	// The system's own lookup, no stand-in: localhost is ::1 over IPv6 and 127.0.0.1 over IPv4 on a machine that has both.
	if (!FFlockSockets::CarriesIpv6())
	{
		AddInfo(TEXT("Skipped: this machine's sockets carry no IPv6."));
		return true;
	}
	const TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> OverIpv6 = FFlockNameLookup::Start(TEXT("localhost"), FFlockNameLookup::FLookUp(), /*bIpv6*/ true);
	const TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> OverIpv4 = FFlockNameLookup::Start(TEXT("localhost"), FFlockNameLookup::FLookUp(), /*bIpv6*/ false);
	const double Until = FPlatformTime::Seconds() + 5.0;
	while ((!OverIpv6->IsDone() || !OverIpv4->IsDone()) && FPlatformTime::Seconds() < Until)
	{
		FPlatformProcess::Sleep(0.01f);
	}
	TestEqualSensitive(TEXT("the IPv6 address, as an IPv6 socket is given it"), OverIpv6->GetAddress(), FString(TEXT("::1")));
	TestEqualSensitive(TEXT("the IPv4 address"), OverIpv4->GetAddress(), FString(TEXT("127.0.0.1")));
	TestTrue(TEXT("an IPv4 address written as the host has no IPv6 one"), FFlockNameLookup::Start(TEXT("127.0.0.1"), FFlockNameLookup::FLookUp(), true)->GetAddress().IsEmpty());
	return true;
}

#endif
