// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Multiplayer/FlockSockets.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Multiplayer/Relay/FlockRelayStream.h"
#include "Multiplayer/Relay/FlockRelayThreads.h"
#include "Multiplayer/Relay/FlockRelayTls.h"
#include "Multiplayer/Relay/FlockRelayWays.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/Support/FlockFakeRelayCertificate.h"
#include "Tests/Support/FlockFakeRelayServer.h"
#include "Tests/Support/FlockRelayFixture.h"
#include "Tests/Support/FlockSessionRelayFixture.h"
#include "Tests/Support/FlockTestSpelling.h"

// The relay reached over TCP and TLS, for a network that blocks UDP to it, against the fake relay's stream sides: the stream's
// framing, padding and waits, a stream the relay ends, sends that never wait, TLS checked by the platform; then the race of ways
// in (UDP, then TCP, then TLS), alone and as a session opens its relay.

#define FLOCK_RELAY_STREAM_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Relay." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace FlockRelayStreamTestHelpers
{
	using namespace FlockRelayTestHelpers;

	/** Quick waits, and every name found on loopback. */
	FFlockRelayTiming OnLoopback()
	{
		FFlockRelayTiming Timing = Quick();
		Timing.NameLookUpForTesting = EveryNameOnLoopback();
		return Timing;
	}

	bool StreamsReady(FAutomationTestBase& Test, const FFlockFakeRelayServer& Relay)
	{
		return Test.TestTrue(TEXT("precondition: the fake relay takes TCP and TLS"), Relay.IsReady());
	}

	/** The relay Login opens, or null with the test told why. */
	TSharedPtr<FFlockRelayConnection> OpenedBy(FAutomationTestBase& Test, FRelayFixture& F, const FFlockRelayLogin& Login, const FFlockRelayTiming& Timing = OnLoopback())
	{
		const TSharedRef<FOpening> Answer = F.Open(Login, Timing);
		F.Until([&Answer]() { return Answer->bAnswered; });
		Test.TestTrue(FString::Printf(TEXT("precondition: the relay opened at %s (%s)"), *Login.GetWay(), *Answer->Result.Failure.Error.Message), Answer->Result.bSuccess);
		return Answer->Result.bSuccess ? Answer->Result.Value : nullptr;
	}

	/** What Login answers when it does not open, after up to Seconds. */
	FFlockRelayFailure FailureOf(FAutomationTestBase& Test, FRelayFixture& F, const FFlockRelayLogin& Login, const FFlockRelayTiming& Timing, double Seconds = 15.0)
	{
		const TSharedRef<FOpening> Answer = F.Open(Login, Timing);
		F.Until([&Answer]() { return Answer->bAnswered; }, Seconds);
		Test.TestTrue(FString::Printf(TEXT("%s was answered"), *Login.GetWay()), Answer->bAnswered);
		Test.TestFalse(FString::Printf(TEXT("%s did not open"), *Login.GetWay()), Answer->Result.bSuccess);
		return Answer->Result.Failure;
	}

	/** Both opened to the host's address, and each keeping a channel to the other when bWithChannels. */
	bool OpenedToEachOther(FAutomationTestBase& Test, FRelayFixture& F, const TSharedPtr<FFlockRelayConnection>& Host, const TSharedPtr<FFlockRelayConnection>& Player,
		bool bWithChannels)
	{
		const TSharedRef<FOpenedTo> HostOpen = F.OpenTo(Host, Host->GetAddress());
		const TSharedRef<FOpenedTo> PlayerOpen = F.OpenTo(Player, Host->GetAddress());
		F.Until([&]() { return HostOpen->bAnswered && PlayerOpen->bAnswered; });
		if (!Test.TestTrue(TEXT("precondition: both opened to the host's relay address"), HostOpen->Result.bSuccess && PlayerOpen->Result.bSuccess))
		{
			return false;
		}
		if (!bWithChannels)
		{
			return true;
		}
		Player->KeepChannelTo(Host->GetAddress());
		Host->KeepChannelTo(Player->GetAddress());
		return Test.TestTrue(TEXT("precondition: channels bound both ways"), F.Until([&]()
		{
			return Player->ChannelNumberForTesting(Host->GetAddress()) != 0 && Host->ChannelNumberForTesting(Player->GetAddress()) != 0
				&& F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num() >= 2;
		}, 5.0));
	}

	bool OneLine(const FString& Text) { return !Text.Contains(TEXT("\n")) && !Text.Contains(TEXT("\r")); }

	int32 Occurrences(const FString& Text, const FString& Part)
	{
		int32 Found = 0;
		for (int32 At = Text.Find(Part, ESearchCase::CaseSensitive); At != INDEX_NONE; At = Text.Find(Part, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + Part.Len()))
		{
			++Found;
		}
		return Found;
	}

	/** A TCP port of this machine's loopback nothing listens on: a connection to it is refused. */
	int32 PortNothingListensOn()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FSocket* Probe = Sockets->CreateSocket(NAME_Stream, TEXT("Flock test free port"), FNetworkProtocolTypes::IPv4);
		const TSharedRef<FInternetAddr> At = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		bool bValid = false;
		At->SetIp(TEXT("127.0.0.1"), bValid);
		At->SetPort(0);
		int32 Port = 0;
		if (Probe != nullptr && Probe->Bind(*At))
		{
			Port = Probe->GetPortNo();
		}
		if (Probe != nullptr)
		{
			Sockets->DestroySocket(Probe);
		}
		return Port;
	}
}

using namespace FlockRelayStreamTestHelpers;

// ---- one way over a stream ----

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamTcpReservesTest, "Stream.OverTcpReservesAnAddressAfterTheLoginChallenge")
bool FFlockRelayStreamTcpReservesTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedBy(*this, F, F.Login(RelayPlayerA, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp));
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("reached over TCP"), static_cast<int32>(Relay->GetTransport()), static_cast<int32>(EFlockRelayTransport::Tcp));
	TestEqualSensitive(TEXT("named as the way it came in by"), Relay->GetWay(), FString::Printf(TEXT("127.0.0.1:%d over TCP"), F.Relay->GetTcpPort()));
	const TArray<FFlockFakeRelayServer::FSeen> Reservations = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate);
	TestEqual(TEXT("challenged, then signed"), Reservations.Num(), 2);
	TestTrue(TEXT("both over the stream"), Reservations.Num() == 2 && Reservations[0].bOverAStream && Reservations[1].bOverAStream && !Reservations[0].bSigned && Reservations[1].bSigned);
	TestFalse(TEXT("nothing over UDP"), F.Relay->AllSeen().ContainsByPredicate([](const FFlockFakeRelayServer::FSeen& Seen) { return !Seen.bOverAStream; }));
	TestEqual(TEXT("one reservation"), F.Relay->Reservations(), 1);
	TestEqualSensitive(TEXT("an IPv4 relay address"), Relay->GetAddress().Ip(), FString(FFlockFakeRelayServer::RelayIp));
	TestEqual(TEXT("the server's endpoint is the way's"), Relay->GetServerEndpoint().Port, F.Relay->GetTcpPort());
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamRequestOnceTest, "Stream.OverAStreamARequestGoesOnceEvenWhileItsAnswerIsSlow")
bool FFlockRelayStreamRequestOnceTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	// Every answer leaves 1.2 s late: over UDP the signed reservation would go again at 0.5 s and 1.5 s, under one transaction.
	F.Relay->AnswerDelaySeconds = 1.2;
	FFlockRelayTiming Timing = OnLoopback();
	Timing.AnswerWaitSeconds = 4.0;
	const TSharedPtr<FFlockRelayConnection> OverTcp = OpenedBy(*this, F, F.Login(RelayPlayerA, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp), Timing);
	const int32 SignedOverTcp = F.Signed(FFlockFakeRelayServer::MethodAllocate);
	TestEqual(TEXT("the signed reservation went once over the stream"), SignedOverTcp, 1);
	const TSharedPtr<FFlockRelayConnection> OverUdp = OpenedBy(*this, F, F.Login(RelayPlayerB), Timing);
	TestTrue(FString::Printf(TEXT("control: over UDP the same slow answer has it sent again (%d in all)"), F.Signed(FFlockFakeRelayServer::MethodAllocate)),
		F.Signed(FFlockFakeRelayServer::MethodAllocate) - SignedOverTcp > 1);
	return OverTcp.IsValid() && OverUdp.IsValid();
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamUdpAndTcpTest, "Stream.PacketsGoBothWaysBetweenUdpAndTcpWithDataNotAMultipleOfFour")
bool FFlockRelayStreamUdpAndTcpTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Host = OpenedBy(*this, F, F.Login(RelayPlayerA));
	const TSharedPtr<FFlockRelayConnection> Player = OpenedBy(*this, F, F.Login(RelayPlayerB, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp));
	if (!Host.IsValid() || !Player.IsValid() || !OpenedToEachOther(*this, F, Host, Player, /*bWithChannels*/ false))
	{
		return false;
	}
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("abcde"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("fghij"));
	if (!OpenedToEachOther(*this, F, Host, Player, /*bWithChannels*/ true))
	{
		return false;
	}
	// Five, nineteen and one bytes: a channel frame on the stream carries padding to 4 bytes, both ways.
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("12345"));
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("on a channel, 19 b."));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("back on a channel 1"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("x"));
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("and after it"));
	TestTrue(TEXT("the player's packets went as channel frames over the stream"), F.Relay->AllSeen().ContainsByPredicate([](const FFlockFakeRelayServer::FSeen& Seen)
	{
		return Seen.Method == FFlockFakeRelayServer::ChannelFrame && Seen.bOverAStream;
	}));
	TestEqual(TEXT("every byte on the stream was a whole message"), F.Relay->UnreadableStreamBytes(), 0);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamUntrustedTest, "Stream.OverTlsThePlatformRefusesACertificateItDoesNotTrust")
bool FFlockRelayStreamUntrustedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay) || !TestTrue(TEXT("precondition: this build speaks TLS"), FFlockRelayTls::IsAvailable()))
	{
		return false;
	}
	// The platform's own trust alone: the fake relay's certificate is its own, as a network that inspects encrypted traffic shows.
	const FFlockRelayFailure Failure = FailureOf(*this, F, F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tls), OnLoopback());
	TestEqualSensitive(TEXT("unreachable"), Failure.Reason, FString(FlockRelayFailureReasons::Unreachable));
	TestTrue(FString::Printf(TEXT("says the certificate is not trusted, over TLS (%s)"), *Failure.Error.Message),
		Failure.Error.Message.Contains(TEXT("certificate this device does not trust")) && Failure.Error.Message.Contains(TEXT("over TLS")));
	TestTrue(TEXT("on one line"), OneLine(Failure.Error.Message));
	TestEqual(TEXT("no reservation"), F.Relay->Reservations(), 0);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamOtherNameTest, "Stream.OverTlsACertificateThatNamesAnotherServerIsRefused")
bool FFlockRelayStreamOtherNameTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay) || !TestTrue(TEXT("precondition: this build speaks TLS"), FFlockRelayTls::IsAvailable()))
	{
		return false;
	}
	// Trusted, but for relay.test, localhost and 127.0.0.1: a relay named otherwise that shows it is someone else.
	FFlockRelayTls::SetTrustedCertificateForTesting(FlockFakeRelayCertificate::Certificate);
	const FFlockRelayFailure Failure = FailureOf(*this, F, F.Login(RelayPlayerA, TEXT("other.test"), EFlockRelayTransport::Tls), OnLoopback());
	TestTrue(FString::Printf(TEXT("refused as not trusted for that name (%s)"), *Failure.Error.Message), Failure.Error.Message.Contains(TEXT("does not trust")));
	const TSharedPtr<FFlockRelayConnection> Named = OpenedBy(*this, F, F.Login(RelayPlayerB, TEXT("relay.test"), EFlockRelayTransport::Tls));
	TestTrue(TEXT("control: the name it carries opens"), Named.IsValid());
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamUdpAndTlsTest, "Stream.PacketsGoBothWaysBetweenUdpAndTls")
bool FFlockRelayStreamUdpAndTlsTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay) || !TestTrue(TEXT("precondition: this build speaks TLS"), FFlockRelayTls::IsAvailable()))
	{
		return false;
	}
	FFlockRelayTls::SetTrustedCertificateForTesting(FlockFakeRelayCertificate::Certificate);
	const TSharedPtr<FFlockRelayConnection> Host = OpenedBy(*this, F, F.Login(RelayPlayerA));
	const TSharedPtr<FFlockRelayConnection> Player = OpenedBy(*this, F, F.Login(RelayPlayerB, TEXT("relay.test"), EFlockRelayTransport::Tls));
	if (!Host.IsValid() || !Player.IsValid() || !OpenedToEachOther(*this, F, Host, Player, /*bWithChannels*/ false))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the player is on TLS"), Player->GetWay(), FString::Printf(TEXT("relay.test:%d over TLS"), F.Relay->GetTlsPort()));
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("over TLS to UDP"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("UDP to TLS"));
	if (!OpenedToEachOther(*this, F, Host, Player, /*bWithChannels*/ true))
	{
		return false;
	}
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("abc"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("a channel back, 17"));
	TestEqual(TEXT("every byte TLS carried was a whole message"), F.Relay->UnreadableStreamBytes(), 0);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamSilentTest, "Stream.ARelayThatTakesTheConnectionAndAnswersNothingIsGivenUpWithinItsWaits")
bool FFlockRelayStreamSilentTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	F.Relay->bStreamsSilent = true;
	FFlockRelayTiming Timing = OnLoopback();
	Timing.ConnectWaitSeconds = 1.0;
	Timing.AnswerWaitSeconds = 1.0;
	double Started = FPlatformTime::Seconds();
	const FFlockRelayFailure OverTls = FailureOf(*this, F, F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tls), Timing);
	const double TlsTook = FPlatformTime::Seconds() - Started;
	TestTrue(FString::Printf(TEXT("TLS: the handshake no answer came to is given up at its wait (%s)"), *OverTls.Error.Message),
		OverTls.Error.Message.Contains(TEXT("did not finish the TLS handshake within 1.0 s")));
	TestTrue(FString::Printf(TEXT("TLS: within its wait (%.1f s)"), TlsTook), TlsTook < 2.5);
	Started = FPlatformTime::Seconds();
	const FFlockRelayFailure OverTcp = FailureOf(*this, F, F.Login(RelayPlayerB, TEXT("relay.test"), EFlockRelayTransport::Tcp), Timing);
	const double TcpTook = FPlatformTime::Seconds() - Started;
	TestTrue(FString::Printf(TEXT("TCP: the reservation no answer came to is given up at the answer wait (%s)"), *OverTcp.Error.Message),
		OverTcp.Error.Message.Contains(TEXT("did not answer within 1.0 s")));
	TestTrue(FString::Printf(TEXT("TCP: within its wait (%.1f s)"), TcpTook), TcpTook < 2.5);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamClosedTest, "Stream.TheRelayClosingTheStreamIsToldLostAtOnce")
bool FFlockRelayStreamClosedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedBy(*this, F, F.Login(RelayPlayerA, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp));
	if (!Relay.IsValid())
	{
		return false;
	}
	const double Closed = FPlatformTime::Seconds();
	F.Relay->CloseEveryStream();
	F.Until([&]() { return Relay->HasStopped(); }, 5.0);
	const double Took = FPlatformTime::Seconds() - Closed;
	TestEqualSensitive(TEXT("lost"), Relay->GetFailureReason(), FString(FlockRelayFailureReasons::Lost));
	TestTrue(FString::Printf(TEXT("told at once (%.1f s), not after the relay's silence (45 s)"), Took), Took < 3.0);
	TestTrue(TEXT("its warning names the way and why"), F.Log->Warnings.ContainsByPredicate([](const FString& Line)
	{
		return Line.Contains(TEXT("over TCP was lost")) && Line.Contains(TEXT("the relay closed the connection"));
	}));
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamResetTest, "Stream.TheRelayResettingTheStreamIsToldLostInOneLineThenItsFix")
bool FFlockRelayStreamResetTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedBy(*this, F, F.Login(RelayPlayerA, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp));
	if (!Relay.IsValid())
	{
		return false;
	}
	F.Relay->ResetEveryStream();
	F.Until([&]() { return Relay->HasStopped(); }, 5.0);
	TestEqualSensitive(TEXT("lost"), Relay->GetFailureReason(), FString(FlockRelayFailureReasons::Lost));
	const FString* Warning = F.Log->Warnings.FindByPredicate([](const FString& Line) { return Line.Contains(TEXT("over TCP was lost")); });
	if (!TestNotNull(TEXT("warned"), Warning))
	{
		return false;
	}
	const FString Fix = FlockRelayFailureReasons::FixFor(FlockRelayFailureReasons::Lost);
	const int32 FixAt = Warning->Find(Fix, ESearchCase::CaseSensitive);
	TestTrue(FString::Printf(TEXT("the fix follows the cause (%s)"), **Warning), FixAt != INDEX_NONE);
	// The cause is the first line; the fix has a line of its own.
	FString Cause;
	Warning->Split(TEXT("\n"), &Cause, nullptr);
	TestTrue(FString::Printf(TEXT("the cause, the platform's words included, is the whole first line (%s)"), **Warning),
		Cause.Contains(TEXT("the connection to the relay broke (")) && Cause.EndsWith(TEXT(").")) && FixAt > Cause.Len());
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamCloseTest, "Stream.CloseOverTcpGivesTheAddressBackAndEndsTheConnection")
bool FFlockRelayStreamCloseTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedBy(*this, F, F.Login(RelayPlayerA, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp));
	if (!Relay.IsValid())
	{
		return false;
	}
	Relay->Close();
	F.Until([&]() { return F.Relay->Releases() == 1 && F.Relay->Reservations() == 0 && F.Relay->StreamConnections() == 0; }, 3.0);
	TestEqual(TEXT("the release went out on the stream before it closed"), F.Relay->Releases(), 1);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	TestEqual(TEXT("and the connection ended"), F.Relay->StreamConnections(), 0);
	TestTrue(TEXT("a close of its own is no loss"), Relay->GetFailureReason().IsEmpty());
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamReleaseFirstTest, "Stream.CloseKeepsTheStreamOpenUntilTheReleaseHasGoneOut")
bool FFlockRelayStreamReleaseFirstTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedBy(*this, F, F.Login(RelayPlayerA, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp));
	if (!Relay.IsValid())
	{
		return false;
	}
	// The sending thread is held, as a slow network would hold it: the release waits in its room after the close.
	FFlockRelayStream::bHoldsSendsForTesting = true;
	Relay->Close();
	F.RealSeconds(0.3);
	TestEqual(TEXT("precondition: the release has not gone yet"), F.Relay->Releases(), 0);
	FFlockRelayStream::bHoldsSendsForTesting = false;
	F.Until([&]() { return F.Relay->Releases() == 1 && F.Relay->StreamConnections() == 0; }, 3.0);
	TestEqual(TEXT("the release went out once it could: the stream stayed open for it"), F.Relay->Releases(), 1);
	TestEqual(TEXT("then the connection ended"), F.Relay->StreamConnections(), 0);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamFullRoomTest, "Stream.SendOnAStreamTheRelayStoppedReadingNeverWaitsAndDropsWholePackets")
bool FFlockRelayStreamFullRoomTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Host = OpenedBy(*this, F, F.Login(RelayPlayerA));
	const TSharedPtr<FFlockRelayConnection> Player = OpenedBy(*this, F, F.Login(RelayPlayerB, TEXT("127.0.0.1"), EFlockRelayTransport::Tcp));
	if (!Host.IsValid() || !Player.IsValid() || !OpenedToEachOther(*this, F, Host, Player, /*bWithChannels*/ false))
	{
		return false;
	}
	// The relay stops reading: the network's buffers fill, then the sending room; a send must still never wait.
	F.Relay->bPauseStreamReading = true;
	uint8 Payload[FFlockRelayConnection::MostBytesInAPacket];
	FMemory::Memset(Payload, 7, sizeof(Payload));
	const FFlockRelayPeer HostAddress = Host->GetAddress();
	int32 Accepted = 0;
	int32 Dropped = 0;
	double Longest = 0.0;
	for (int32 Index = 0; Index < 30000 && Dropped < 200; ++Index)
	{
		const double Before = FPlatformTime::Seconds();
		const bool bSent = Player->Send(HostAddress, Payload, UE_ARRAY_COUNT(Payload));
		Longest = FMath::Max(Longest, FPlatformTime::Seconds() - Before);
		bSent ? ++Accepted : ++Dropped;
	}
	TestTrue(FString::Printf(TEXT("a full room dropped packets (%d accepted, %d dropped)"), Accepted, Dropped), Dropped > 0);
	TestTrue(FString::Printf(TEXT("no send waited on the network (the longest took %.1f ms)"), Longest * 1000.0), Longest < 0.01);
	F.Relay->bPauseStreamReading = false;
	// Every packet the room took reached the relay whole: none was cut, so nothing after it was misread.
	F.Until([&]() { return F.Relay->Count(FFlockFakeRelayServer::MethodSend, 0x010) >= Accepted; }, 20.0);
	TestEqual(TEXT("every packet taken reached the relay"), F.Relay->Count(FFlockFakeRelayServer::MethodSend, 0x010), Accepted);
	TestEqual(TEXT("as whole messages"), F.Relay->UnreadableStreamBytes(), 0);
	TestTrue(TEXT("the relay is still open"), Player->IsOpen());
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayStreamNoSendingThreadTest, "Stream.NoThreadToSendOnTheWayFailsNoThread")
bool FFlockRelayStreamNoSendingThreadTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	// The relay's own thread runs; the stream's sending thread is refused, as threads would be in a launch without them.
	FFlockRelayTiming Timing = Quick();
	Timing.NameLookUpForTesting = [](const FString&, TFunction<void(const TArray<FString>&)> Answer)
	{
		FFlockRelayThreads::bRefusedForTesting = true;
		Answer({ TEXT("127.0.0.1") });
	};
	const FFlockRelayFailure Failure = FailureOf(*this, F, F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp), Timing);
	FFlockRelayThreads::bRefusedForTesting = false;
	TestEqualSensitive(TEXT("no thread"), Failure.Reason, FString(FlockRelayFailureReasons::NoThread));
	TestTrue(FString::Printf(TEXT("says so (%s)"), *Failure.Error.Message), Failure.Error.Message.Contains(TEXT("no thread could be started to send to it")));
	F.Until([&]() { return F.Relay->StreamConnections() == 0; }, 3.0);
	TestEqual(TEXT("its connection was closed"), F.Relay->StreamConnections(), 0);
	return true;
}

// ---- the race of ways in ----

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysEndAsItOpensTest, "Ways.TheSessionEndingAsAWayOpensGivesThatWayBack")
bool FFlockRelayWaysEndAsItOpensTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.Race({ F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) }, OnLoopback());
	const TSharedRef<FFlockRelayWays> Race = F.Races.Last();
	// The session ends at the moment the way opened, before the race hands it out.
	FFlockRelayWays::AfterAWayOpenedForTesting = [Race]() { Race->Close(); };
	F.Until([&]() { return Opening->bAnswered; });
	TestTrue(TEXT("ended as cancelled"), Opening->bAnswered && !Opening->Result.bSuccess && Opening->Result.Failure.IsCancelled());
	// Left open, nobody would hold it, and its reservation would be renewed for as long as the game ran.
	F.Until([&]() { return F.Relay->Releases() >= 1 && F.Relay->Reservations() == 0; }, 3.0);
	TestTrue(TEXT("the way that opened was given back"), F.Relay->Releases() >= 1 && F.Relay->Reservations() == 0);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysEndWhileOpeningTest, "Ways.ASessionThatEndsWhileAWayOpensStartsNoOtherWay")
bool FFlockRelayWaysEndWhileOpeningTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	F.Relay->bIgnoresUdp = true;
	FFlockRelayTiming Timing = OnLoopback();
	Timing.SocketRoundWaitSeconds = 0.2;
	const TSharedRef<FOpening> Opening = F.Race({ F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) }, Timing);
	F.RealSeconds(0.3);
	if (!TestFalse(TEXT("precondition: UDP is still trying"), Opening->bAnswered))
	{
		return false;
	}
	F.Races.Last()->Close();
	TestTrue(TEXT("ended as cancelled"), Opening->bAnswered && Opening->Result.Failure.IsCancelled());
	F.RealSeconds(2.5);
	TestEqual(TEXT("the UDP way ended by the session's end started no TCP way"), F.Relay->StreamsAccepted(), 0);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysSameKindTest, "Ways.AnotherPortOfTheSameKindStartsHalfASecondAfterOneNothingAnswers")
bool FFlockRelayWaysSameKindTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// Two UDP ports of one server, as Flock lists UDP on 3478 and 443: the first unanswered, the second answering.
	FFlockFakeRelayServer Silent;
	Silent.bSilent = true;
	FFlockRelayLogin First = F.Login(RelayPlayerA);
	First.Port = Silent.GetPort();
	const double Started = FPlatformTime::Seconds();
	const TSharedRef<FOpening> Opening = F.Race({ First, F.Login(RelayPlayerA) }, OnLoopback());
	F.Until([&]() { return Opening->bAnswered; });
	const double Took = FPlatformTime::Seconds() - Started;
	if (!TestTrue(FString::Printf(TEXT("opened (%s)"), *Opening->Result.Failure.Error.Message), Opening->Result.bSuccess))
	{
		return false;
	}
	TestEqualSensitive(TEXT("on the port that answers"), Opening->Result.Value->GetWay(), FString::Printf(TEXT("127.0.0.1:%d over UDP"), F.Relay->GetPort()));
	TestTrue(FString::Printf(TEXT("the second port started half a second after the first, not the 2 s another kind waits (%.2f s)"), Took), Took > 0.4 && Took < 1.6);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysTellsEveryWayTest, "Ways.NoWayOpensTellsEveryWayAndTheReasonOfAWayThatReachedTheServer")
bool FFlockRelayWaysTellsEveryWayTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	F.Relay->bIgnoresUdp = true;
	F.Relay->Password = TEXT("not-what-flock-minted");
	FFlockRelayTiming Timing = OnLoopback();
	Timing.SocketRoundWaitSeconds = 0.2;
	const TSharedRef<FOpening> Opening = F.Race({ F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) }, Timing);
	F.Until([&]() { return Opening->bAnswered; }, 15.0);
	const FFlockRelayFailure& Failure = Opening->Result.Failure;
	TestEqualSensitive(TEXT("the server answered over TCP: its refusal says more than UDP's silence"), Failure.Reason, FString(FlockRelayFailureReasons::WrongLogin));
	const FString Told = Failure.Error.ReasonAndFix();
	TestTrue(FString::Printf(TEXT("names the UDP way and the TCP way (%s)"), *Failure.Error.Message),
		Failure.Error.Message.Contains(TEXT("over UDP")) && Failure.Error.Message.Contains(TEXT("over TCP")));
	TestEqual(FString::Printf(TEXT("one fix, the told reason's, after every way's cause (%s)"), *Told),
		Occurrences(Told, FlockRelayFailureReasons::FixFor(FlockRelayFailureReasons::WrongLogin)), 1);
	TestEqual(FString::Printf(TEXT("said once that no way opened (%s)"), *Told), Occurrences(Told, TEXT("No way in to Flock's relay opened.")), 1);
	TestTrue(TEXT("every way's cause on one line"), OneLine(Failure.Error.Message));
	TestEqualSensitive(TEXT("the fix for the reason told"), Failure.Error.Hint, FlockRelayFailureReasons::FixFor(FlockRelayFailureReasons::WrongLogin));
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysOneThreadRefusalTest, "Ways.NoThreadForOneWayIsToldOnceRatherThanByEveryWay")
bool FFlockRelayWaysOneThreadRefusalTest::RunTest(const FString& Parameters)
{
	FRelayFixture F(/*bWithStreams*/ true);
	if (!StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	FFlockRelayThreads::bRefusedForTesting = true;
	const TSharedRef<FOpening> Opening = F.Race({ F.Login(RelayPlayerA, TEXT("relay.test")), F.Login(RelayPlayerA, TEXT("relay.test"), EFlockRelayTransport::Tcp) }, OnLoopback());
	F.Until([&]() { return Opening->bAnswered; }, 5.0);
	FFlockRelayThreads::bRefusedForTesting = false;
	TestEqualSensitive(TEXT("no thread"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::NoThread));
	TestEqual(TEXT("one way tried: no other could run either"), F.Races.Last()->GetWaysStartedForTesting().Num(), 1);
	return true;
}

// ---- as a session opens its relay ----

namespace FlockRelayStreamTestHelpers
{
	/** A session's relay over the fake relay with streams, every name found on loopback, the test certificate trusted; each test lists the ways (Udp, Tcp, Tls). */
	struct FSessionWaysFixture : FSessionRelayFixture
	{
		FSessionWaysFixture()
			: FSessionRelayFixture(0, /*bWithStreams*/ true)
		{
			FFlockRelayTls::SetTrustedCertificateForTesting(FlockFakeRelayCertificate::Certificate);
			Sessions().SetRelayTimingForTesting(OnLoopback());
		}

		void SetTiming(FFlockRelayTiming Timing)
		{
			Timing.NameLookUpForTesting = EveryNameOnLoopback();
			Sessions().SetRelayTimingForTesting(Timing);
		}

		FString Udp() const { return Relay->UrlNamed(TEXT("relay.test")); }
		FString Tcp() const { return Relay->TcpUrlNamed(TEXT("relay.test")); }
		FString Tls() const { return Relay->TlsUrlNamed(TEXT("relay.test")); }
	};

	TSharedPtr<FFlockRelayConnection> OpenedInTheSession(FAutomationTestBase& Test, FSessionWaysFixture& F, UFlockMultiplayerSession* Session, double& OutSeconds,
		const FString& OnServer = FString())
	{
		const double Started = FPlatformTime::Seconds();
		const TSharedRef<FOpening> Opening = F.OpenRelay(Session, OnServer);
		F.RelayFramesUntil([&]() { return Opening->bAnswered; }, 15.0);
		OutSeconds = FPlatformTime::Seconds() - Started;
		Test.TestTrue(FString::Printf(TEXT("the session's relay opened (%s)"), *Opening->Result.Failure.Error.Message), Opening->Result.bSuccess);
		return Opening->Result.bSuccess ? Opening->Result.Value : nullptr;
	}
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysUdpBlockedTest, "Ways.Session.UdpBlockedOpensOverTcpAfterTheNextWayWaitNotUdpsOwnWaits")
bool FFlockRelayWaysUdpBlockedTest::RunTest(const FString& Parameters)
{
	FSessionWaysFixture F;
	F.Server->RelayUrls = { F.Udp(), F.Tcp() };
	F.Relay->bIgnoresUdp = true;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr || !StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedInTheSession(*this, F, Session, Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("over TCP"), static_cast<int32>(Relay->GetTransport()), static_cast<int32>(EFlockRelayTransport::Tcp));
	TestTrue(FString::Printf(TEXT("once UDP had had its 2 s, not after its three socket rounds and its answer wait (%.1f s)"), Took), Took > 1.8 && Took < 4.5);
	const TArray<FString> Said = F.Log->Infos.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("relay is reached")); });
	TestEqual(TEXT("said once why the relay is reached over TCP"), Said.Num(), 1);
	TestTrue(TEXT("and why"), Said.Num() == 1 && Said[0].Contains(TEXT("over TCP")) && Said[0].Contains(TEXT("blocks UDP")) && Said[0].Contains(TEXT("stutter")));
	TestFalse(TEXT("at Info, not as a warning"), F.Log->Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("relay is reached")); }));
	F.RealSeconds(1.0);
	TestEqual(TEXT("one reservation, the one kept"), F.Relay->Reservations(), 1);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysUdpAndTcpBlockedTest, "Ways.Session.UdpAndTcpBlockedOpensOverTls")
bool FFlockRelayWaysUdpAndTcpBlockedTest::RunTest(const FString& Parameters)
{
	FSessionWaysFixture F;
	// TCP to a port nothing listens on, as a network that turns TCP to the relay away.
	F.Server->RelayUrls = { F.Udp(), FString::Printf(TEXT("turn:relay.test:%d?transport=tcp"), PortNothingListensOn()), F.Tls() };
	F.Relay->bIgnoresUdp = true;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr || !StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedInTheSession(*this, F, Session, Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("over TLS"), Relay->GetWay(), FString::Printf(TEXT("relay.test:%d over TLS"), F.Relay->GetTlsPort()));
	TestTrue(FString::Printf(TEXT("within two ways' waits (%.1f s)"), Took), Took < 7.0);
	// The debug log says when each way was tried and how each ended, so a slow opening on a player's network can be read off it.
	const auto DebugsSaying = [&F](const FString& Text) { return F.Log->Debugs.FilterByPredicate([&Text](const FString& Line) { return Line.Contains(Text); }).Num(); };
	for (const TCHAR* Over : { TEXT("UDP"), TEXT("TCP"), TEXT("TLS") })
	{
		TestEqual(FString::Printf(TEXT("the way over %s on IPv4 said once as it was tried"), Over), DebugsSaying(FString::Printf(TEXT("over %s ("), Over)), 1);
	}
	TestEqual(TEXT("the TLS way's opening said once"), DebugsSaying(FString::Printf(TEXT("%s opened after"), *Relay->GetWay())), 1);
	const TArray<FString> Failed = F.Log->Debugs.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("did not open after")); });
	TestTrue(TEXT("every way said to have failed says why"), Failed.FilterByPredicate([](const FString& Line)
		{
			const int32 Cause = Line.Find(TEXT("): "));
			return Cause == INDEX_NONE || Line.Len() <= Cause + 3;
		}).Num() == 0);
	TSet<FString> FailedWays;
	for (const FString& Line : Failed)
	{
		FailedWays.Add(Line.Left(Line.Find(TEXT(" did not open after"))));
	}
	TestEqual(TEXT("and says it once"), FailedWays.Num(), Failed.Num());
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysUdpAnswersTest, "Ways.Session.UdpAnswersNoStreamIsEverOpened")
bool FFlockRelayWaysUdpAnswersTest::RunTest(const FString& Parameters)
{
	FSessionWaysFixture F;
	F.Server->RelayUrls = { F.Tls(), F.Tcp(), F.Udp() };
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr || !StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedInTheSession(*this, F, Session, Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("UDP first, wherever Flock lists it"), static_cast<int32>(Relay->GetTransport()), static_cast<int32>(EFlockRelayTransport::Udp));
	F.RealSeconds(2.5);
	TestEqual(TEXT("no TCP or TLS connection was made while UDP worked"), F.Relay->StreamsAccepted(), 0);
	TestEqual(TEXT("one reservation"), F.Relay->Reservations(), 1);
	TestFalse(TEXT("nothing to say when the relay opens as it always did"), F.Log->Infos.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("relay is reached")); }));
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysStillOpeningTest, "Ways.Session.AWayStillOpeningWhenAnotherIsKeptGivesItsAddressBack")
bool FFlockRelayWaysStillOpeningTest::RunTest(const FString& Parameters)
{
	// The UDP way's signed reservation is held unanswered, so TCP opens first; the held answer is let go afterwards.
	FFlockFakeRelayServer Slow;
	Slow.bHoldReservationAnswers = true;
	FSessionWaysFixture F;
	F.Server->RelayUrls = { Slow.UrlNamed(TEXT("slow.relay.test")), F.Tcp() };
	// The UDP way's own answer wait outlasts the TCP way's start at 2 s, so it is still opening when TCP is kept, rather than
	// giving up (and its address back) on its own first.
	FFlockRelayTiming Timing = Quick();
	Timing.AnswerWaitSeconds = 6.0;
	F.SetTiming(Timing);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr || !StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedInTheSession(*this, F, Session, Took);
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("precondition: TCP opened while the UDP way waited for its answer"), static_cast<int32>(Relay->GetTransport()), static_cast<int32>(EFlockRelayTransport::Tcp));
	TestTrue(FString::Printf(TEXT("precondition: kept before the UDP way's own wait could end (%.1f s)"), Took), Took < 5.5);
	TestTrue(TEXT("precondition: the UDP way had asked for a reservation"), Slow.SeenRequests(FFlockFakeRelayServer::MethodAllocate).ContainsByPredicate([](const FFlockFakeRelayServer::FSeen& Seen) { return Seen.bSigned; }));
	F.RealSeconds(0.5);
	// Left open, the UDP way would now open on this answer and keep its address, renewed, for the whole session.
	Slow.ReleaseHeldAnswers();
	F.RealSeconds(2.0);
	TestEqual(TEXT("the UDP way's address was given back"), Slow.Reservations(), 0);
	TestTrue(TEXT("by its release"), Slow.Releases() >= 1);
	TestEqual(TEXT("the kept one stays"), F.Relay->Reservations(), 1);
	TestTrue(TEXT("and is open"), Relay->IsOpen());
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysHostsServerTest, "Ways.Session.OnTheHostsServerByAnotherWayIn")
bool FFlockRelayWaysHostsServerTest::RunTest(const FString& Parameters)
{
	FSessionWaysFixture F;
	F.Server->RelayUrls = { F.Udp(), F.Tcp() };
	F.SignIn(PlayerB);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr || !StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	F.Relay->bIgnoresUdp = true;
	// The host reserved over UDP and published that way; this player's network blocks UDP.
	double Took = 0.0;
	const TSharedPtr<FFlockRelayConnection> Relay = OpenedInTheSession(*this, F, Session, Took, FString::Printf(TEXT("relay.test:%d"), F.Relay->GetPort()));
	if (!Relay.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("on the host's server, over TCP"), static_cast<int32>(Relay->GetTransport()), static_cast<int32>(EFlockRelayTransport::Tcp));
	const TSharedRef<FOpening> Again = F.OpenRelay(Session, FString::Printf(TEXT("RELAY.TEST:%d"), F.Relay->GetTlsPort()));
	TestTrue(TEXT("the relay already on that server is handed back, whatever way it came in by and whatever port is named"), Again->bAnswered && Again->Result.Value == Relay);
	TestEqual(TEXT("one reservation"), F.Relay->Reservations(), 1);
	return true;
}

FLOCK_RELAY_STREAM_TEST(FFlockRelayWaysSessionEveryWayTest, "Ways.Session.EveryWayBlockedEndsUnreachableNamingEachWayOnOneLine")
bool FFlockRelayWaysSessionEveryWayTest::RunTest(const FString& Parameters)
{
	FSessionWaysFixture F;
	F.Server->RelayUrls = { F.Udp(), F.Tcp(), F.Tls() };
	F.Relay->bIgnoresUdp = true;
	F.Relay->bStreamsSilent = true;
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.2;
	Timing.AnswerWaitSeconds = 1.0;
	Timing.ConnectWaitSeconds = 1.0;
	F.SetTiming(Timing);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr || !StreamsReady(*this, *F.Relay))
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; }, 20.0);
	const FFlockRelayFailure& Failure = Opening->Result.Failure;
	TestEqualSensitive(TEXT("unreachable"), Failure.Reason, FString(FlockRelayFailureReasons::Unreachable));
	TestTrue(FString::Printf(TEXT("names UDP, TCP and TLS (%s)"), *Failure.Error.Message),
		Failure.Error.Message.Contains(TEXT("over UDP")) && Failure.Error.Message.Contains(TEXT("over TCP")) && Failure.Error.Message.Contains(TEXT("over TLS")));
	TestTrue(TEXT("on one line"), OneLine(Failure.Error.Message));
	TestTrue(TEXT("its fix names UDP, TCP and TLS"), Failure.Error.Hint.Contains(TEXT("UDP, TCP or TLS")));
	return true;
}

#endif
