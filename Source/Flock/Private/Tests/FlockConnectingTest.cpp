// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Http/FlockHttpClient.h"
#include "IPAddress.h"
#include "Misc/Base64.h"
#include "Multiplayer/FlockDirectAddresses.h"
#include "Multiplayer/FlockMultiplayerSession.h"
#include "Multiplayer/FlockMultiplayerSessions.h"
#include "Multiplayer/FlockTravelToSession.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Providers/FlockMultiplayerProvider.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/Support/FlockConnectingFixture.h"
#include "Tests/Support/FlockFakeSessionServer.h"
#include "Tests/Support/FlockFakeStunServer.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockScriptedGameTravel.h"
#include "Tests/Support/FlockSessionTestListener.h"
#include "Tests/Support/FlockTestSpelling.h"
#include "UObject/StrongObjectPtr.h"

namespace FlockConnectingTestHelpers
{
	/** The protocol this machine gives a dual-stack socket, asked of the socket layer itself: IPv6 where it has one, as on the desktops this runs on. */
	inline FName ProtocolThisMachineGives()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		FSocket* Probe = Sockets->CreateSocket(NAME_DGram, TEXT("Flock test probe"), FNetworkProtocolTypes::IPv6);
		if (Probe == nullptr)
		{
			return FNetworkProtocolTypes::IPv4;
		}
		Sockets->DestroySocket(Probe);
		return FNetworkProtocolTypes::IPv6;
	}
}

using namespace FlockConnectingTestHelpers;

#define FLOCK_CONNECTING_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Connecting." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

// ---- the host publishes ----

FLOCK_CONNECTING_TEST(FFlockConnectingPublishTest, "PublishDirectPublishesThePublicAddressThePortAndBothAddresses")
bool FFlockConnectingPublishTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	if (!TestTrue(TEXT("precondition: a STUN server on loopback"), F.Stun->IsReady()))
	{
		return false;
	}
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Published = F.Publish(Session, 7777);
	F.FramesUntil([&]() { return Published->bAnswered; });
	if (!TestTrue(TEXT("published"), Published->Succeeded()))
	{
		AddInfo(Published->Result.Error.Message);
		return false;
	}
	const TSharedPtr<FJsonObject> Sent = F.LastPublished();
	TestEqualSensitive(TEXT("mode"), F.Field(Sent, TEXT("mode")), FString(TEXT("direct")));
	TestEqualSensitive(TEXT("the address players outside the LAN use"), F.Field(Sent, TEXT("address")), HostsPublicAddress);
	const TSharedPtr<FJsonValue> Port = Sent.IsValid() ? Sent->TryGetField(TEXT("port")) : nullptr;
	TestTrue(TEXT("the port is a number on the wire"), Port.IsValid() && Port->Type == EJson::Number && Port->AsNumber() == 7777.0);
	TestEqualSensitive(TEXT("the public address"), F.Field(Sent, TEXT("public_address")), HostsPublicAddress);
	TestEqualSensitive(TEXT("routed toward the STUN server, here on loopback"), F.Field(Sent, TEXT("lan_address")), FString(TEXT("127.0.0.1")));
	TestEqual(TEXT("one STUN request"), F.Stun->Requests, 1);
	TestTrue(TEXT("asked from a dual-stack socket, which a phone on mobile data needs"),
		F.Sessions().GetDirectAddressesForTesting().GetAskedFromForTesting() == ProtocolThisMachineGives());
	const TArray<FFlockHttpRequest> Relay = F.Server->RequestsTo(ERoute::RelayCredentials);
	if (TestEqual(TEXT("the STUN servers were read once"), Relay.Num(), 1))
	{
		TestEqualSensitive(TEXT("no session named: a direct connection is not relay use"), Relay[0].JsonBody, FString(TEXT("{}")));
		TestEqualSensitive(TEXT("read as the signed-in player"), FFlockFakePartyServer::PlayerOf(Relay[0]), PlayerA);
	}
	TestEqualSensitive(TEXT("the session holds what it published"), Session->GetConnection().Mode, FString(TEXT("direct")));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingNoIpv6Test, "WithoutIpv6PublishDirectStillPublishesThePublicAddress")
bool FFlockConnectingNoIpv6Test::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockUdpSockets::bDualStackRefusedForTesting = true;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Published = F.Publish(Session, 7777);
	F.FramesUntil([&]() { return Published->bAnswered; });
	TestTrue(TEXT("published"), Published->Succeeded());
	TestEqualSensitive(TEXT("asked from a plain IPv4 socket, as before"), F.Field(F.LastPublished(), TEXT("public_address")), HostsPublicAddress);
	TestTrue(TEXT("the IPv4 socket"), F.Sessions().GetDirectAddressesForTesting().GetAskedFromForTesting() == FNetworkProtocolTypes::IPv4);
	TestTrue(TEXT("the refusal is remembered for the launch"), FFlockUdpSockets::RemembersARefusalForTesting());
	TestEqual(TEXT("one STUN request"), F.Stun->Requests, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingDualStackSendTest, "ADualStackSocketThatCannotSendToIpv4IsReplacedByAPlainIpv4One")
bool FFlockConnectingDualStackSendTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	if (ProtocolThisMachineGives() != FNetworkProtocolTypes::IPv6)
	{
		AddInfo(TEXT("This machine makes no dual-stack sockets: nothing to replace."));
		return true;
	}
	FFlockUdpSockets::bDualStackSendRefusedForTesting = true;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Published = F.Publish(Session, 7777);
	F.FramesUntil([&]() { return Published->bAnswered; });
	TestTrue(TEXT("published"), Published->Succeeded());
	TestEqualSensitive(TEXT("the public address, asked from the IPv4 socket"), F.Field(F.LastPublished(), TEXT("public_address")), HostsPublicAddress);
	TestTrue(TEXT("the IPv4 socket"), F.Sessions().GetDirectAddressesForTesting().GetAskedFromForTesting() == FNetworkProtocolTypes::IPv4);
	TestEqual(TEXT("one STUN request reached the server"), F.Stun->Requests, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingSecondPublishTest, "ASecondPublishReadsTheStunServersOnceAndAsksTheStunServerAgain")
bool FFlockConnectingSecondPublishTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> First = F.Publish(Session, 7777);
	F.FramesUntil([&]() { return First->bAnswered; });
	F.Stun->PublicAddress = TEXT("198.51.100.4");
	const TSharedRef<FSessionAnswer> Second = F.Publish(Session, 7778);
	F.FramesUntil([&]() { return Second->bAnswered; });
	TestTrue(TEXT("published again"), Second->Succeeded());
	TestEqual(TEXT("the STUN servers are read once a launch"), F.Server->Count(ERoute::RelayCredentials), 1);
	TestEqual(TEXT("a public address can change, so it is asked each time"), F.Stun->Requests, 2);
	TestEqualSensitive(TEXT("the new address"), F.Field(F.LastPublished(), TEXT("address")), FString(TEXT("198.51.100.4")));
	int32 Port = 0;
	TestTrue(TEXT("the new port"), F.LastPublished().IsValid() && F.LastPublished()->TryGetNumberField(TEXT("port"), Port) && Port == 7778);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingSilentStunTest, "ASilentStunServerPublishesTheLanAddressAloneWarnedOnce")
bool FFlockConnectingSilentStunTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.Stun->bSilent = true;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> First = F.Publish(Session, 7777);
	F.Frame();
	TestFalse(TEXT("still waiting on the STUN server"), First->bAnswered);
	F.Advance(0.6);
	TestTrue(TEXT("published once the STUN wait ran out"), First->Succeeded());
	const TSharedRef<FSessionAnswer> Second = F.Publish(Session, 7777);
	F.Advance(0.6);
	TestTrue(TEXT("published again"), Second->Succeeded());
	const TSharedPtr<FJsonObject> Sent = F.LastPublished();
	TestEqualSensitive(TEXT("the LAN address when the public one is unknown"), F.Field(Sent, TEXT("address")), FString(TEXT("127.0.0.1")));
	TestEqualSensitive(TEXT("the LAN address"), F.Field(Sent, TEXT("lan_address")), FString(TEXT("127.0.0.1")));
	TestFalse(TEXT("an unknown address is left out, not sent empty"), Sent.IsValid() && Sent->HasField(TEXT("public_address")));
	TestEqual(TEXT("warned once"), F.WarningsContaining(TEXT("public address could not be found")), 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingRelayRouteFailingTest, "TheRelayRouteFailingStillPublishesTheLanAddressAndIsAskedAgainNextTime")
bool FFlockConnectingRelayRouteFailingTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::RelayCredentials, FFlockFakeSessionServer::Refused(400, TEXT("request.validation_failed")));
	const TSharedRef<FSessionAnswer> First = F.Publish(Session, 7777);
	F.FramesUntil([&]() { return First->bAnswered; });
	TestTrue(TEXT("published"), First->Succeeded());
	TestFalse(TEXT("no public address"), F.LastPublished().IsValid() && F.LastPublished()->HasField(TEXT("public_address")));
	TestEqual(TEXT("no STUN server known"), F.Stun->Requests, 0);
	TestTrue(TEXT("routed toward the internet instead"), F.Field(F.LastPublished(), TEXT("lan_address")).Contains(TEXT(".")));
	TestEqual(TEXT("warned that the servers could not be read"), F.WarningsContaining(TEXT("STUN servers could not be read")), 1);

	const TSharedRef<FSessionAnswer> Second = F.Publish(Session, 7777);
	F.FramesUntil([&]() { return Second->bAnswered; });
	TestEqual(TEXT("a failed read is not kept"), F.Server->Count(ERoute::RelayCredentials), 2);
	TestEqualSensitive(TEXT("the public address once read"), F.Field(F.LastPublished(), TEXT("address")), HostsPublicAddress);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingLookUpNeverAnswersTest, "ANameLookUpThatNeverAnswersLeavesThePublicAddressUnknownWithinTheStunWait")
bool FFlockConnectingLookUpNeverAnswersTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.Server->StunUrls = { TEXT("stun:stun.example.test:19302") };
	F.Sessions().GetDirectAddressesForTesting().SetNameLookUpForTesting([](const FString&, TFunction<void(const FString&)>) {});
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Published = F.Publish(Session, 7777);
	F.Frame();
	TestFalse(TEXT("precondition: waiting on the name"), Published->bAnswered);
	F.Advance(0.6);
	TestTrue(TEXT("published without the name ever resolving"), Published->Succeeded());
	TestFalse(TEXT("no public address"), F.LastPublished().IsValid() && F.LastPublished()->HasField(TEXT("public_address")));
	TestEqualSensitive(TEXT("the LAN address alone"), F.Field(F.LastPublished(), TEXT("address")), F.Field(F.LastPublished(), TEXT("lan_address")));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingCancelPublishTest, "CancellingEndsAPublishWaitingOnANameLookUp")
bool FFlockConnectingCancelPublishTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.Sessions().GetDirectAddressesForTesting().SetStunWaitForTesting(30.0);
	F.Server->StunUrls = { TEXT("stun:stun.example.test:19302") };
	F.Sessions().GetDirectAddressesForTesting().SetNameLookUpForTesting([](const FString&, TFunction<void(const FString&)>) {});
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Published = MakeShared<FSessionAnswer>();
	FFlockRequestHandle Handle = Session->PublishDirectConnection(7777, Into(Published));
	F.Advance(0.3);
	TestFalse(TEXT("precondition: waiting on the name"), Published->bAnswered);
	Handle.Cancel();
	TestTrue(TEXT("answered as cancelled at once"), Published->FailedAs(EFlockErrorType::Cancelled));
	F.Advance(1.0);
	TestEqual(TEXT("answered once"), Published->Times, 1);
	TestEqual(TEXT("nothing published"), F.Server->Count(ERoute::Publish), 0);
	TestEqual(TEXT("no search left running"), F.Sessions().GetDirectAddressesForTesting().GetSearchCountForTesting(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingPortRangeTest, "APortOutOfRangeIsRefusedBeforeAnyRequest")
bool FFlockConnectingPortRangeTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const int32 Before = F.Server->Requests.Num();
	for (const int32 Port : { 0, -1, 65536 })
	{
		const TSharedRef<FSessionAnswer> Refused = F.Publish(Session, Port);
		TestTrue(FString::Printf(TEXT("%d refused at once"), Port), Refused->FailedAs(EFlockErrorType::Validation)
			&& Refused->Result.Error.Message.Contains(TEXT("from 1 to 65535")));
	}
	TestEqual(TEXT("no STUN request for a refused port"), F.Stun->Requests, 0);
	const TSharedRef<FSessionAnswer> Edge = F.Publish(Session, 65535);
	F.FramesUntil([&]() { return Edge->bAnswered; });
	TestTrue(TEXT("65535 is a port"), Edge->Succeeded());
	TestEqual(TEXT("the relay route and one publish"), F.Server->Requests.Num(), Before + 2);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingSignInEndsPublishTest, "ASignInThatEndsWhileTheServersAreReadPublishesNothing")
bool FFlockConnectingSignInEndsPublishTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Held = F.Server->HoldNext(ERoute::RelayCredentials);
	const TSharedRef<FSessionAnswer> Published = F.Publish(Session, 7777);
	F.SignIn(PlayerB);
	Held->Release();
	F.FramesUntil([&]() { return Published->bAnswered; });
	TestTrue(TEXT("the call ended cancelled: its sign-in ended"), Published->FailedAs(EFlockErrorType::Cancelled));
	TestEqual(TEXT("nothing published as anyone"), F.Server->Count(ERoute::Publish), 0);
	TestEqual(TEXT("no STUN request"), F.Stun->Requests, 0);
	return true;
}

// ---- a player waits for the host ----

FLOCK_CONNECTING_TEST(FFlockConnectingWaitLearnsTest, "AWaitingPlayerLearnsTheHostsAddressFromItsOwnReadsNotOnlyHeartbeats")
bool FFlockConnectingWaitLearnsTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 8.f);
	F.Advance(0.5);
	TestFalse(TEXT("nothing published yet"), Waiting->bAnswered);
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.25);
	if (!TestTrue(TEXT("learned well before a heartbeat"), Waiting->Succeeded()))
	{
		return false;
	}
	TestTrue(TEXT("the session, holding the connection"), Waiting->Result.Value == Session && Session->HasConnection());
	TestEqualSensitive(TEXT("direct"), Session->GetConnection().Mode, FString(TEXT("direct")));
	TestTrue(TEXT("read while waiting"), F.Server->Count(ERoute::Read) >= 1);
	TestEqual(TEXT("no heartbeat yet"), F.Server->Count(ERoute::Heartbeat), 0);
	const int32 Reads = F.Server->Count(ERoute::Read);
	F.Advance(0.3);
	F.Advance(0.3);
	TestEqual(TEXT("no reads once nobody waits"), F.Server->Count(ERoute::Read), Reads);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingHeardFirstTest, "TheGameHearsTheConnectionChangeBeforeAWaitIsAnswered")
bool FFlockConnectingHeardFirstTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 8.f);
	UFlockSessionTestListener* Listener = NewObject<UFlockSessionTestListener>();
	const TStrongObjectPtr<UFlockSessionTestListener> KeepListener(Listener);
	Listener->Listen(Session);
	TOptional<bool> AnsweredWhenHeard;
	Listener->AlsoInHandler = [&AnsweredWhenHeard, Waiting]()
	{
		if (!AnsweredWhenHeard.IsSet())
		{
			AnsweredWhenHeard = Waiting->bAnswered;
		}
	};
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.25);
	TestEqual(TEXT("the game heard the connection change once"), Listener->ConnectionChanged, 1);
	TestTrue(TEXT("with the session already holding it, before the wait was answered"),
		AnsweredWhenHeard.IsSet() && !AnsweredWhenHeard.GetValue() && Listener->ConnectionSeen.Num() == 1 && Listener->ConnectionSeen[0]);
	TestTrue(TEXT("then the wait was answered"), Waiting->Succeeded());
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingAlreadyPublishedTest, "AlreadyPublishedIsAnsweredAtOnceWithoutARead")
bool FFlockConnectingAlreadyPublishedTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	const TSharedRef<FSessionAnswer> Read = MakeShared<FSessionAnswer>();
	F.Provider->GetSession(Session->GetId(), Into(Read));
	const int32 Reads = F.Server->Count(ERoute::Read);
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 5.f);
	TestTrue(TEXT("answered at once"), Waiting->Succeeded() && Session->HasConnection());
	F.Advance(0.5);
	TestEqual(TEXT("no read started"), F.Server->Count(ERoute::Read), Reads);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingWaitRunsOutTest, "AWaitThatRunsOutOfTimeAnswersTheSessionWithNoConnectionAndTheSessionGoesOn")
bool FFlockConnectingWaitRunsOutTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 0.6f);
	F.Advance(0.3);
	TestFalse(TEXT("still waiting"), Waiting->bAnswered);
	F.Advance(0.4);
	TestTrue(TEXT("answered"), Waiting->Succeeded() && Waiting->Result.Value == Session);
	TestFalse(TEXT("no connection"), Session->HasConnection());
	TestFalse(TEXT("only the wait ended"), Session->HasEnded());
	const int32 Reads = F.Server->Count(ERoute::Read);
	F.Advance(0.3);
	F.Advance(0.3);
	TestEqual(TEXT("the reads stopped with the wait"), F.Server->Count(ERoute::Read), Reads);
	TestFalse(TEXT("the waiting read is stopped"), F.Sessions().GetConnectionWaitForTesting().IsValid());
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingWaitSessionEndsTest, "TheSessionEndingWhileWaitingAnswersItEndedWithTheReason")
bool FFlockConnectingWaitSessionEndsTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 8.f);
	F.Server->Ends(Served);
	F.Advance(0.25);
	TestTrue(TEXT("the end was read"), Waiting->Succeeded() && Waiting->Result.Value == Session);
	TestEqualSensitive(TEXT("with the reason"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::EndedByHost));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingWaitSignInEndsTest, "ASignInThatEndsWhileWaitingAnswersTheSessionSignedOutAndStopsTheReads")
bool FFlockConnectingWaitSignInEndsTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 8.f);
	F.SignIn(PlayerC);
	F.Frame();
	TestTrue(TEXT("answered"), Waiting->Succeeded());
	TestEqualSensitive(TEXT("the session ended signed out"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	const int32 Reads = F.Server->Count(ERoute::Read);
	F.Advance(0.3);
	F.Advance(0.3);
	TestEqual(TEXT("nothing more is read, as anyone"), F.Server->Count(ERoute::Read), Reads);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingTwoWaitsTest, "TwoWaitsShareOneReadAndBothGetTheConnection")
bool FFlockConnectingTwoWaitsTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.Sessions().SetConnectionWaitSecondsForTesting(0.5);
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> First = F.Wait(Session, 8.f);
	const TSharedRef<FSessionAnswer> Second = F.Wait(Session, 8.f);
	for (int32 Step = 0; Step < 13; ++Step)
	{
		F.Advance(0.1);
	}
	TestTrue(TEXT("one read every half second, not two"), F.Server->Count(ERoute::Read) <= 3);
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.5);
	TestTrue(TEXT("both learned"), First->Succeeded() && Second->Succeeded());
	TestTrue(TEXT("the same session"), First->Result.Value == Second->Result.Value && Session->HasConnection());
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingClearedAddressTest, "AnAddressPublishedThenClearedBetweenTwoReadsKeepsTheWaitGoingUntilTheNewHostPublishes")
bool FFlockConnectingClearedAddressTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Joins(Served, PlayerC);
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 8.f);
	// Published, then hosting moved on, before this game reads again: the reading holds a moved epoch and no address.
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Server->MakesHost(Served, PlayerC);
	for (int32 Step = 0; Step < 5; ++Step)
	{
		F.Advance(0.2);
	}
	TestFalse(TEXT("still waiting: the new host has published nothing yet"), Waiting->bAnswered);
	TestFalse(TEXT("precondition: the reading cleared the address"), Session->HasConnection());
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7778), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	TestTrue(TEXT("the new host's address"), Waiting->Succeeded() && Session->HasConnection());
	int32 Port = 0;
	TestTrue(TEXT("its port"), Session->GetConnection().Values.TryGetInt(TEXT("port"), Port) && Port == 7778);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingCancelWaitTest, "CancellingEndsTheWaitAndItsReads")
bool FFlockConnectingCancelWaitTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	FFlockRequestHandle Handle;
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 8.f, &Handle);
	const TSharedRef<FSessionAnswer> Other = F.Wait(Session, 8.f);
	F.Advance(0.3);
	Handle.Cancel();
	TestTrue(TEXT("given up"), Waiting->FailedAs(EFlockErrorType::Cancelled));
	TestFalse(TEXT("the other wait goes on"), Other->bAnswered);
	const int32 ReadsWhileOtherWaits = F.Server->Count(ERoute::Read);
	F.Advance(0.25);
	TestTrue(TEXT("still read for the other wait"), F.Server->Count(ERoute::Read) > ReadsWhileOtherWaits);
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.25);
	TestTrue(TEXT("the other wait learned"), Other->Succeeded());
	TestEqual(TEXT("the cancelled wait answered once"), Waiting->Times, 1);
	const int32 Reads = F.Server->Count(ERoute::Read);
	F.Advance(0.3);
	TestEqual(TEXT("no reads after both"), F.Server->Count(ERoute::Read), Reads);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingShutdownWaitTest, "FlockShuttingDownEndsTheWaitAsCancelled")
bool FFlockConnectingShutdownWaitTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 30.f);
	F.Advance(0.3);
	F.Provider->Shutdown();
	TestTrue(TEXT("ended as cancelled, as every call does at shutdown"), Waiting->FailedAs(EFlockErrorType::Cancelled));
	TestEqual(TEXT("answered once"), Waiting->Times, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingWaitRefusedTest, "AWaitOfNoLengthOrOnAnEndedSessionIsRefused")
bool FFlockConnectingWaitRefusedTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Zero = F.Wait(Session, 0.f);
	TestTrue(TEXT("no length refused"), Zero->FailedAs(EFlockErrorType::Validation) && Zero->Result.Error.Message.Contains(TEXT("longer than zero")));
	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Session->Leave(Into(Left));
	const TSharedRef<FSessionAnswer> Ended = F.Wait(Session, 5.f);
	TestTrue(TEXT("an ended session refused"), Ended->FailedAs(EFlockErrorType::Validation) && Ended->Result.Error.Message.Contains(TEXT("has ended")));
	TestEqual(TEXT("no read for either"), F.Server->Count(ERoute::Read), 0);
	return true;
}

// ---- which of the host's addresses a player connects to ----

namespace FlockConnectingTestHelpers
{
	/** Joins A's session as B, has A publish Published, and finds the address this game uses. */
	inline TSharedRef<FAddressAnswer> Found(FAutomationTestBase& Test, FConnectingFixture& F, const TSharedRef<FJsonObject>& Published)
	{
		const TSharedRef<FAddressAnswer> Answer = MakeShared<FAddressAnswer>();
		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
		UFlockMultiplayerSession* Session = F.JoinedAsB(Test, Served);
		if (Session == nullptr)
		{
			return Answer;
		}
		F.Server->Publishes(Served, Published);
		const TSharedRef<FSessionAnswer> Waiting = F.Wait(Session, 5.f);
		F.Advance(0.25);
		Test.TestTrue(TEXT("precondition: published"), Waiting->Succeeded());
		Session->FindDirectAddress(Into(Answer));
		F.FramesUntil([&]() { return Answer->bAnswered; });
		if (!Answer->bAnswered)
		{
			// A silent STUN server is waited out on the fixture's clock.
			F.Advance(0.6);
		}
		Test.TestTrue(TEXT("found"), Answer->Succeeded());
		return Answer;
	}
}

FLOCK_CONNECTING_TEST(FFlockConnectingSharesPublicTest, "APlayerSharingTheHostsPublicAddressConnectsToItsLanAddress")
bool FFlockConnectingSharesPublicTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	const TSharedRef<FAddressAnswer> Answer = Found(*this, F, Direct(Text(HostsPublicAddress), Number(7777)));
	TestEqualSensitive(TEXT("the LAN address"), Answer->Result.Value.Address, FString(TEXT("192.168.1.20")));
	TestEqual(TEXT("the port"), Answer->Result.Value.Port, 7777);
	TestTrue(TEXT("on the LAN"), Answer->Result.Value.bIsLan);
	TestEqual(TEXT("this device's own public address was asked"), F.Stun->Requests, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingElsewhereTest, "APlayerElsewhereConnectsToThePublishedAddress")
bool FFlockConnectingElsewhereTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	const TSharedRef<FAddressAnswer> Answer = Found(*this, F,
		Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	TestEqualSensitive(TEXT("the published address"), Answer->Result.Value.Address, FString(TEXT("198.51.100.4")));
	TestFalse(TEXT("not the LAN"), Answer->Result.Value.bIsLan);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingUnknownPublicTest, "APlayerWhosePublicAddressIsUnknownConnectsToThePublishedAddress")
bool FFlockConnectingUnknownPublicTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.Stun->bSilent = true;
	const TSharedRef<FAddressAnswer> Answer = Found(*this, F, Direct(Text(HostsPublicAddress), Number(7777)));
	TestEqualSensitive(TEXT("not the LAN address: this device may be elsewhere"), Answer->Result.Value.Address, HostsPublicAddress);
	TestFalse(TEXT("not the LAN"), Answer->Result.Value.bIsLan);
	TestEqual(TEXT("asked, and no answer came"), F.Stun->Requests, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingLanOnlyHostTest, "AHostThatKnewOnlyItsLanAddressIsReachedThere")
bool FFlockConnectingLanOnlyHostTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	const TSharedRef<FAddressAnswer> Answer = Found(*this, F, Direct(Text(TEXT("192.168.1.20")), Number(7777), TEXT("192.168.1.20"), FString()));
	TestEqualSensitive(TEXT("the LAN address"), Answer->Result.Value.Address, FString(TEXT("192.168.1.20")));
	TestTrue(TEXT("the only address the host knew"), Answer->Result.Value.bIsLan);
	TestEqual(TEXT("nothing to compare, so nothing asked"), F.Stun->Requests, 0);
	TestEqual(TEXT("and the STUN servers not read"), F.Server->Count(ERoute::RelayCredentials), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingPortAsTextTest, "APortPublishedAsTextIsStillAPort")
bool FFlockConnectingPortAsTextTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	const TSharedRef<FAddressAnswer> Answer = Found(*this, F,
		Direct(Text(TEXT("198.51.100.4")), Text(TEXT("7777")), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	TestEqual(TEXT("the port"), Answer->Result.Value.Port, 7777);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingUnusableTest, "ADescriptorPlayersCannotUseFindsNoAddress")
bool FFlockConnectingUnusableTest::RunTest(const FString& Parameters)
{
	TArray<TSharedRef<FJsonObject>> Unusable;
	{
		const TSharedRef<FJsonObject> Steam = MakeShared<FJsonObject>();
		Steam->SetStringField(TEXT("mode"), TEXT("steam"));
		Steam->SetStringField(TEXT("steam_id"), TEXT("76561198000000000"));
		Unusable.Add(Steam);
		const TSharedRef<FJsonObject> NoPort = MakeShared<FJsonObject>();
		NoPort->SetStringField(TEXT("mode"), TEXT("direct"));
		NoPort->SetStringField(TEXT("address"), TEXT("198.51.100.4"));
		Unusable.Add(NoPort);
		Unusable.Add(Direct(Text(TEXT("198.51.100.4")), Number(0), FString(), FString()));
		Unusable.Add(Direct(Text(TEXT("198.51.100.4")), Number(65536), FString(), FString()));
		Unusable.Add(Direct(Text(TEXT("198.51.100.4")), Number(99999999999.0), FString(), FString()));
		Unusable.Add(Direct(Text(TEXT("198.51.100.4")), Number(7777.5), FString(), FString()));
		Unusable.Add(Direct(Text(TEXT("7")), Number(7777), FString(), FString()));
		Unusable.Add(Direct(Text(TEXT("host.example")), Number(7777), FString(), FString()));
		Unusable.Add(Direct(Text(TEXT("::1")), Number(7777), FString(), FString()));
		// An address with a space was written wrongly somewhere: refused, never trimmed.
		Unusable.Add(Direct(Text(TEXT(" 198.51.100.4")), Number(7777), FString(), FString()));
		const TSharedRef<FJsonObject> Capital = MakeShared<FJsonObject>();
		Capital->SetStringField(TEXT("mode"), TEXT("Direct"));
		Capital->SetStringField(TEXT("address"), TEXT("198.51.100.4"));
		Capital->SetNumberField(TEXT("port"), 7777);
		Unusable.Add(Capital);
	}
	for (const TSharedRef<FJsonObject>& Descriptor : Unusable)
	{
		FConnectingFixture F;
		FString Written;
		FJsonSerializer::Serialize(Descriptor, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Written));
		const TSharedRef<FAddressAnswer> Answer = Found(*this, F, Descriptor);
		TestFalse(FString::Printf(TEXT("no address in %s"), *Written), Answer->Result.Value.IsFound());
	}
	return true;
}

// ---- reading a STUN server's answer ----

namespace FlockConnectingTestHelpers
{
	const TArray<uint8> Transaction = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

	/** A binding answer of the given type holding the given attributes, each already padded to four bytes. */
	inline TArray<uint8> StunAnswer(uint16 Type, const TArray<uint8>& Of, const TArray<TArray<uint8>>& Attributes)
	{
		int32 Length = 0;
		for (const TArray<uint8>& Attribute : Attributes)
		{
			Length += Attribute.Num();
		}
		TArray<uint8> Answer;
		Answer.SetNumZeroed(20);
		Answer[0] = static_cast<uint8>(Type >> 8);
		Answer[1] = static_cast<uint8>(Type);
		Answer[2] = static_cast<uint8>(Length >> 8);
		Answer[3] = static_cast<uint8>(Length);
		Answer[4] = 0x21;
		Answer[5] = 0x12;
		Answer[6] = 0xA4;
		Answer[7] = 0x42;
		for (int32 Index = 0; Index < 12; ++Index)
		{
			Answer[8 + Index] = Of[Index];
		}
		for (const TArray<uint8>& Attribute : Attributes)
		{
			Answer.Append(Attribute);
		}
		return Answer;
	}

	/** 203.0.113.7:50000, masked with the magic cookie as XOR-MAPPED-ADDRESS carries it. */
	inline TArray<uint8> XorMapped(uint8 A = 203, uint8 B = 0, uint8 C = 113, uint8 D = 7)
	{
		const int32 Port = 50000 ^ 0x2112;
		return { 0x00, 0x20, 0x00, 0x08, 0x00, 0x01, static_cast<uint8>(Port >> 8), static_cast<uint8>(Port),
			static_cast<uint8>(A ^ 0x21), static_cast<uint8>(B ^ 0x12), static_cast<uint8>(C ^ 0xA4), static_cast<uint8>(D ^ 0x42) };
	}

	inline TArray<uint8> PlainMapped(uint8 A, uint8 B, uint8 C, uint8 D) { return { 0x00, 0x01, 0x00, 0x08, 0x00, 0x01, 0xC3, 0x50, A, B, C, D }; }

	/** An attribute the reader does not use, five bytes long so its padding is read too. */
	inline TArray<uint8> Software() { return { 0x80, 0x22, 0x00, 0x05, 't', 'e', 's', 't', '!', 0, 0, 0 }; }
}

FLOCK_CONNECTING_TEST(FFlockConnectingStunMaskedTest, "StunAnswer.TheMaskedAddressIsReadAndPreferredOverThePlainOneWhereverItSits")
bool FFlockConnectingStunMaskedTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("the masked address"),
		FFlockDirectAddresses::PublicAddressInStunAnswer(StunAnswer(0x0101, Transaction, { XorMapped() }), Transaction), FString(TEXT("203.0.113.7")));
	TestEqualSensitive(TEXT("preferred over the plain one before it"), FFlockDirectAddresses::PublicAddressInStunAnswer(
		StunAnswer(0x0101, Transaction, { PlainMapped(198, 51, 100, 4), Software(), XorMapped() }), Transaction), FString(TEXT("203.0.113.7")));
	TestEqualSensitive(TEXT("the plain address when it is the only one"), FFlockDirectAddresses::PublicAddressInStunAnswer(
		StunAnswer(0x0101, Transaction, { Software(), PlainMapped(198, 51, 100, 4) }), Transaction), FString(TEXT("198.51.100.4")));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingStunNothingTest, "StunAnswer.AnotherRequestsAnAnErrorBytesCutShortOrAnIpv6AddressReadNothing")
bool FFlockConnectingStunNothingTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Other = Transaction;
	Other[11] = 99;
	TestTrue(TEXT("an answer to another request"), FFlockDirectAddresses::PublicAddressInStunAnswer(StunAnswer(0x0101, Other, { XorMapped() }), Transaction).IsEmpty());
	TestTrue(TEXT("an error answer"), FFlockDirectAddresses::PublicAddressInStunAnswer(StunAnswer(0x0111, Transaction, { XorMapped() }), Transaction).IsEmpty());
	const TArray<uint8> Whole = StunAnswer(0x0101, Transaction, { XorMapped() });
	for (int32 Length = 0; Length < Whole.Num(); ++Length)
	{
		const TArray<uint8> Cut(Whole.GetData(), Length);
		TestTrue(FString::Printf(TEXT("%d bytes"), Length), FFlockDirectAddresses::PublicAddressInStunAnswer(Cut, Transaction).IsEmpty());
	}
	FString Address;
	int32 Port = 0;
	TestFalse(TEXT("no bytes at all"), FFlockDirectAddresses::PublicEndpointInStunAnswer(nullptr, 0, Transaction, Address, Port));
	TArray<uint8> Ipv6;
	Ipv6.SetNumZeroed(24);
	Ipv6[1] = 0x20;
	Ipv6[3] = 20;
	Ipv6[5] = 0x02;
	TestTrue(TEXT("an IPv6 address is left out"), FFlockDirectAddresses::PublicAddressInStunAnswer(StunAnswer(0x0101, Transaction, { Ipv6 }), Transaction).IsEmpty());
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingStunPortTest, "StunAnswer.ThePortIsReadUnmaskedFromEitherAttributeAndOnlyTheBytesReceivedAreRead")
bool FFlockConnectingStunPortTest::RunTest(const FString& Parameters)
{
	FString Address;
	int32 Port = 0;
	const TArray<uint8> Masked = StunAnswer(0x0101, Transaction, { XorMapped() });
	TestTrue(TEXT("masked"), FFlockDirectAddresses::PublicEndpointInStunAnswer(Masked.GetData(), 32, Transaction, Address, Port)
		&& Address == TEXT("203.0.113.7") && Port == 50000);
	const TArray<uint8> Plain = StunAnswer(0x0101, Transaction, { PlainMapped(198, 51, 100, 4) });
	TestTrue(TEXT("plain"), FFlockDirectAddresses::PublicEndpointInStunAnswer(Plain.GetData(), 32, Transaction, Address, Port)
		&& Address == TEXT("198.51.100.4") && Port == 50000);
	TArray<uint8> Buffer = Masked;
	Buffer.AddZeroed(64);
	TestTrue(TEXT("the bytes received"), FFlockDirectAddresses::PublicEndpointInStunAnswer(Buffer.GetData(), Masked.Num(), Transaction, Address, Port)
		&& Address == TEXT("203.0.113.7"));
	TestFalse(TEXT("cut short of the address"), FFlockDirectAddresses::PublicEndpointInStunAnswer(Buffer.GetData(), Masked.Num() - 1, Transaction, Address, Port));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingStunUrlTest, "StunAnswer.OnlyPlainStunServersOnIpv4AreAsked")
bool FFlockConnectingStunUrlTest::RunTest(const FString& Parameters)
{
	FString Host;
	int32 Port = 0;
	TestTrue(TEXT("a name and a port"), FFlockDirectAddresses::ParseStunUrl(TEXT("stun:stun.l.google.com:19302"), Host, Port)
		&& Host == TEXT("stun.l.google.com") && Port == 19302);
	TestTrue(TEXT("no port is 3478"), FFlockDirectAddresses::ParseStunUrl(TEXT("stun:stun.cloudflare.com"), Host, Port) && Port == 3478);
	TestTrue(TEXT("a query is left off"), FFlockDirectAddresses::ParseStunUrl(TEXT("stun:198.51.100.4:3478?transport=udp"), Host, Port)
		&& Host == TEXT("198.51.100.4"));
	TestFalse(TEXT("TLS is not asked"), FFlockDirectAddresses::ParseStunUrl(TEXT("stuns:stun.example.test:5349"), Host, Port));
	TestFalse(TEXT("a relay is not a STUN server"), FFlockDirectAddresses::ParseStunUrl(TEXT("turn:turn.example.test:3478"), Host, Port));
	TestFalse(TEXT("an IPv6 host"), FFlockDirectAddresses::ParseStunUrl(TEXT("stun:[::1]:3478"), Host, Port));
	TestFalse(TEXT("a port out of range"), FFlockDirectAddresses::ParseStunUrl(TEXT("stun:stun.example.test:70000"), Host, Port));
	TestFalse(TEXT("no host"), FFlockDirectAddresses::ParseStunUrl(TEXT("stun::3478"), Host, Port));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockConnectingStunListParseTest, "TheRelayRoutesRealAnswerIsReadForItsStunServersAlone")
bool FFlockConnectingStunListParseTest::RunTest(const FString& Parameters)
{
	// The backend's answer with no session (measured), and a relay entry beside it as an answer for a seat carries one.
	const FString Body = TEXT(R"BODY({"ttl":600,"ice_servers":[{"urls":["stun:stun.l.google.com:19302"],"username":null,"credential":null},)BODY")
		TEXT(R"BODY({"urls":["turn:turn.example.test:3478?transport=udp"],"username":"u","credential":"c"}],"relay_paused":false})BODY");
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Body), Object);
	FFlockStunServerList List;
	FString Error;
	TestTrue(TEXT("read"), Object.IsValid() && FFlockStunServerList::FromWireObject(Object.ToSharedRef(), List, Error));
	TestEqual(TEXT("the STUN server alone"), List.Urls.Num(), 1);
	if (List.Urls.Num() == 1)
	{
		TestEqualSensitive(TEXT("as listed"), List.Urls[0], FString(TEXT("stun:stun.l.google.com:19302")));
	}
	TSharedPtr<FJsonObject> Missing;
	FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(FString(TEXT(R"({"ttl":600})"))), Missing);
	TestFalse(TEXT("an answer listing no servers fails the read"), FFlockStunServerList::FromWireObject(Missing.ToSharedRef(), List, Error));
	return true;
}

// ---- Travel To Session ----

namespace FlockConnectingTestHelpers
{
	/** The host's travel up to its level opening, with nothing published yet. */
	inline TSharedRef<FTravelAnswer> HostTravel(FAutomationTestBase& Test, FConnectingFixture& F, UFlockMultiplayerSession*& OutSession,
		const FFlockTravelToSessionOptions& Options = FFlockTravelToSessionOptions())
	{
		F.SignIn(PlayerA);
		OutSession = F.Hosted(Test);
		if (OutSession == nullptr)
		{
			return MakeShared<FTravelAnswer>();
		}
		return F.Travel(OutSession, Options);
	}

	/** A player's travel, with A's session joined as B. */
	inline TSharedRef<FTravelAnswer> PlayerTravel(FAutomationTestBase& Test, FConnectingFixture& F, FFlockFakeSessionServer::FSessionRef& OutServed,
		const FFlockTravelToSessionOptions& Options = FFlockTravelToSessionOptions(), FFlockRequestHandle* OutHandle = nullptr)
	{
		UFlockMultiplayerSession* Session = F.JoinedAsB(Test, OutServed);
		if (Session == nullptr)
		{
			return MakeShared<FTravelAnswer>();
		}
		return F.Travel(Session, Options, OutHandle);
	}
}

FLOCK_CONNECTING_TEST(FFlockTravelHostTest, "Travel.TheHostOpensItsLevelListeningThenPublishesThePortItListensOn")
bool FFlockTravelHostTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Travel = HostTravel(*this, F, Session);
	if (Session == nullptr)
	{
		return false;
	}
	TestEqualSensitive(TEXT("the level it is in, opened again listening"), FString::Join(F.Game->Opened, TEXT(",")), FString(TEXT("/Game/Maps/Lobby")));
	F.Frame();
	F.Frame();
	TestEqual(TEXT("nothing published while the level opens"), F.Server->Count(ERoute::Publish), 0);
	TestFalse(TEXT("not done"), Travel->bAnswered);
	// The engine took the next free port: another game on this machine listens on 7777 (measured).
	F.Game->Opens(7778);
	F.FramesUntil([&]() { return Travel->bAnswered; });
	if (!TestTrue(TEXT("done"), Travel->Succeeded()))
	{
		AddInfo(Travel->Result.Error.Message);
		return false;
	}
	const FFlockTravelToSessionResult& Result = Travel->Result.Value;
	TestEqualSensitive(TEXT("started as host"), Result.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	TestTrue(TEXT("hosts"), Result.bIsHost);
	TestEqual(TEXT("the port the level listens on"), Result.Port, 7778);
	int32 Published = 0;
	TestTrue(TEXT("published with that port"), F.LastPublished().IsValid() && F.LastPublished()->TryGetNumberField(TEXT("port"), Published) && Published == 7778);
	TestEqualSensitive(TEXT("the address published"), Result.Address, HostsPublicAddress);
	TestEqualSensitive(TEXT("the connection published"), Result.Connection.Mode, FString(TEXT("direct")));
	TestEqual(TEXT("still listening"), F.Game->StoppedListening, 0);
	TestEqual(TEXT("no travel of its own"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelHostLevelTest, "Travel.TheHostOpensTheLevelTheGameNames")
bool FFlockTravelHostLevelTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockTravelToSessionOptions Options;
	Options.HostLevel = TEXT("/Game/Maps/Arena");
	UFlockMultiplayerSession* Session = nullptr;
	HostTravel(*this, F, Session, Options);
	TestEqualSensitive(TEXT("the level named"), FString::Join(F.Game->Opened, TEXT(",")), FString(TEXT("/Game/Maps/Arena")));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelNotListeningTest, "Travel.ALevelThatOpensWithoutListeningCouldNotStartAndPublishesNothing")
bool FFlockTravelNotListeningTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Travel = HostTravel(*this, F, Session);
	F.Game->Opens(0);
	F.Frame();
	TestTrue(TEXT("done"), Travel->Succeeded());
	TestEqualSensitive(TEXT("could not start"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotStart));
	TestEqual(TEXT("nothing published"), F.Server->Count(ERoute::Publish), 0);
	TestEqual(TEXT("warned"), F.WarningsContaining(TEXT("did not open listening")), 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelOpenFailsTest, "Travel.ALevelThatFailsToOpenCouldNotStartWithTheEnginesWords")
bool FFlockTravelOpenFailsTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Travel = HostTravel(*this, F, Session);
	F.Game->Fails(TEXT("ClientTravelFailure: Failed to load package '/Game/Maps/Lobby'"));
	F.Frame();
	TestFalse(TEXT("not done while the engine is still changing level"), Travel->bAnswered);
	// The engine keeps the level it was in.
	F.Game->State.bChangingLevel = false;
	F.Frame();
	TestEqualSensitive(TEXT("could not start"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotStart));
	TestEqual(TEXT("warned with the engine's words"), F.WarningsContaining(TEXT("Failed to load package")), 1);
	TestEqual(TEXT("nothing published"), F.Server->Count(ERoute::Publish), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelPublishRefusedTest, "Travel.AHostWhosePublishIsRefusedStopsListening")
bool FFlockTravelPublishRefusedTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> Travel = HostTravel(*this, F, Session);
	F.Server->AnswerNext(ERoute::Publish, FFlockFakeSessionServer::Refused(403, TEXT("multiplayer.not_host")));
	F.Game->Opens(7777);
	F.FramesUntil([&]() { return Travel->bAnswered; });
	TestTrue(TEXT("failed on the server's code"), Travel->bAnswered && !Travel->Result.bSuccess
		&& Travel->Result.Error.ErrorCode == EFlockErrorCode::MultiplayerNotHost);
	TestEqual(TEXT("nobody can reach a host that did not say where it is: it stopped listening"), F.Game->StoppedListening, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelPlayerTest, "Travel.APlayerWaitsForTheHostAndTravelsToItsLanAddressSharingItsPublicAddress")
bool FFlockTravelPlayerTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	F.Advance(0.5);
	TestEqual(TEXT("no travel before the host publishes"), F.Game->TraveledTo.Num(), 0);
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqualSensitive(TEXT("traveled to the host's LAN address"), FString::Join(F.Game->TraveledTo, TEXT(",")), FString(TEXT("192.168.1.20:7777")));
	F.Frame();
	TestFalse(TEXT("not done while connecting"), Travel->bAnswered);
	F.Game->Arrives(/*bLetIn*/ false);
	F.Frame();
	TestFalse(TEXT("not done before the host lets the player in"), Travel->bAnswered);
	F.Game->Arrives(/*bLetIn*/ true);
	F.Frame();
	if (!TestTrue(TEXT("done"), Travel->Succeeded()))
	{
		return false;
	}
	const FFlockTravelToSessionResult& Result = Travel->Result.Value;
	TestEqualSensitive(TEXT("connected"), Result.Outcome, FString(FlockTravelToSessionOutcomes::Connected));
	TestFalse(TEXT("a player"), Result.bIsHost);
	TestEqualSensitive(TEXT("the address"), Result.Address, FString(TEXT("192.168.1.20")));
	TestEqual(TEXT("the port"), Result.Port, 7777);
	TestEqualSensitive(TEXT("the host's connection"), Result.Connection.Mode, FString(TEXT("direct")));
	TestEqual(TEXT("answered once"), Travel->Times, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelElsewhereTest, "Travel.APlayerElsewhereTravelsToThePublishedAddress")
bool FFlockTravelElsewhereTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	PlayerTravel(*this, F, Served);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqualSensitive(TEXT("the published address"), FString::Join(F.Game->TraveledTo, TEXT(",")), FString(TEXT("198.51.100.4:7777")));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelNeverPublishedTest, "Travel.AHostThatNeverPublishesEndsAsHostNeverPublished")
bool FFlockTravelNeverPublishedTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockTravelToSessionOptions Options;
	Options.WaitForHostSeconds = 5.f;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served, Options);
	F.Advance(4.0);
	TestFalse(TEXT("still waiting"), Travel->bAnswered);
	F.Advance(1.5);
	TestTrue(TEXT("done"), Travel->Succeeded());
	TestEqualSensitive(TEXT("its own outcome"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::HostNeverPublished));
	TestTrue(TEXT("no connection in the result"), Travel->Result.Value.Connection.Mode.IsEmpty());
	TestEqual(TEXT("no travel"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelSessionEndedTest, "Travel.TheSessionEndingWhileWaitingEndsAsSessionEnded")
bool FFlockTravelSessionEndedTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	F.Server->Ends(Served);
	F.Advance(0.25);
	TestEqualSensitive(TEXT("session ended"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::SessionEnded));
	TestEqual(TEXT("no travel"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelNotDirectTest, "Travel.AConnectionOfTheGamesOwnEndsAsNotDirectHoldingIt")
bool FFlockTravelNotDirectTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	const TSharedRef<FJsonObject> Steam = MakeShared<FJsonObject>();
	Steam->SetStringField(TEXT("mode"), TEXT("steam"));
	Steam->SetStringField(TEXT("steam_id"), TEXT("76561198000000000"));
	F.Server->Publishes(Served, Steam);
	F.Advance(0.25);
	TestEqualSensitive(TEXT("not direct"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::NotDirect));
	FString SteamId;
	TestTrue(TEXT("the result holds the game's own connection"), Travel->Result.Value.Connection.Values.TryGetString(TEXT("steam_id"), SteamId)
		&& SteamId == TEXT("76561198000000000"));
	TestEqual(TEXT("no travel"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelUnusableTest, "Travel.AnAddressPlayersCannotUseCouldNotConnectWarned")
bool FFlockTravelUnusableTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	const TSharedRef<FJsonObject> NoPort = MakeShared<FJsonObject>();
	NoPort->SetStringField(TEXT("mode"), TEXT("direct"));
	NoPort->SetStringField(TEXT("address"), TEXT("198.51.100.4"));
	F.Server->Publishes(Served, NoPort);
	F.Advance(0.25);
	TestEqualSensitive(TEXT("could not connect"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestEqual(TEXT("warned"), F.WarningsContaining(TEXT("no usable IPv4 address and port")), 1);
	TestEqual(TEXT("no travel"), F.Game->TraveledTo.Num(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelEngineGivesUpTest, "Travel.AConnectionTheEngineGivesUpCouldNotConnectWarnedWithTheForwardingNote")
bool FFlockTravelEngineGivesUpTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	// As measured: the connection times out with no world, then the engine loads the game's default level.
	F.Game->Fails(TEXT("ConnectionTimeout: UNetConnection::Tick: Connection TIMED OUT."));
	F.Frame();
	TestFalse(TEXT("not done while the engine still drops the connection (measured): the completion runs in the level the game stays in"),
		Travel->bAnswered);
	F.Game->GoesBackToItsDefaultLevel();
	F.Frame();
	TestEqualSensitive(TEXT("could not connect"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestEqual(TEXT("warned with the address, the engine's words and the forwarding note"),
		F.Log->Warnings.FilterByPredicate([](const FString& Line)
		{
			return Line.Contains(TEXT("198.51.100.4:7777")) && Line.Contains(TEXT("TIMED OUT")) && Line.Contains(TEXT("forwarded"));
		}).Num(), 1);
	TestEqual(TEXT("the engine gave the connection up itself"), F.Game->StoppedConnecting, 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelBackWithoutAWordTest, "Travel.BackInALevelOfItsOwnWithoutAWordCouldNotConnect")
bool FFlockTravelBackWithoutAWordTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Frame();
	TestFalse(TEXT("not done while connecting"), Travel->bAnswered);
	F.Game->GoesBackToItsDefaultLevel();
	F.Frame();
	TestEqualSensitive(TEXT("could not connect"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::CouldNotConnect));
	TestEqual(TEXT("no reason given"), F.WarningsContaining(TEXT("no reason given")), 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelMistakesTest, "Travel.MistakesAreRefused_ALevelOpeningIsWaitedFor_AndASecondTravelTakesOver")
bool FFlockTravelMistakesTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	F.bHasWorld = false;
	const TSharedRef<FTravelAnswer> NoWorld = F.Travel(Session);
	TestTrue(TEXT("no world"), NoWorld->FailedAs(EFlockErrorType::Validation) && NoWorld->Result.Error.Message.Contains(TEXT("world")));
	F.bHasWorld = true;

	F.Game->State.NetMode = NM_ListenServer;
	const TSharedRef<FTravelAnswer> Hosting = F.Travel(Session);
	TestTrue(TEXT("already hosting"), Hosting->FailedAs(EFlockErrorType::Validation) && Hosting->Result.Error.Message.Contains(TEXT("already")));
	F.Game->State.NetMode = NM_Standalone;
	F.Game->State.bChangingLevel = true;
	FFlockRequestHandle OpeningHandle;
	const TSharedRef<FTravelAnswer> Opening = F.Travel(Session, FFlockTravelToSessionOptions(), &OpeningHandle);
	TestFalse(TEXT("a level opening is waited for, not refused"), Opening->bAnswered);
	OpeningHandle.Cancel();
	TestTrue(TEXT("and cancelled while it waits"), Opening->FailedAs(EFlockErrorType::Cancelled));
	F.Game->State.bChangingLevel = false;

	FFlockTravelToSessionOptions NoWait;
	NoWait.WaitForHostSeconds = 0.f;
	const TSharedRef<FTravelAnswer> Zero = F.Travel(Session, NoWait);
	TestTrue(TEXT("no time to wait"), Zero->FailedAs(EFlockErrorType::Validation) && Zero->Result.Error.Message.Contains(TEXT("longer than zero")));

	const TSharedRef<FTravelAnswer> First = F.Travel(Session);
	const TSharedRef<FTravelAnswer> Second = F.Travel(Session);
	TestTrue(TEXT("a second while one is under way takes over: the first is restarted"), First->Succeeded()
		&& First->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Restarted, ESearchCase::CaseSensitive));
	TestFalse(TEXT("the second goes on"), Second->bAnswered);

	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Session->Leave(Into(Left));
	TestEqualSensitive(TEXT("the second ended with the session"), Second->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::SessionEnded));
	TestEqual(TEXT("the first answered once"), First->Times, 1);
	const TSharedRef<FTravelAnswer> Ended = F.Travel(Session);
	TestTrue(TEXT("an ended session"), Ended->FailedAs(EFlockErrorType::Validation) && Ended->Result.Error.Message.Contains(TEXT("has ended")));
	TestEqual(TEXT("the engine was asked nothing"), F.Game->TraveledTo.Num() + F.Game->Opened.Num(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelCancelConnectingTest, "Travel.CancellingWhileConnectingGivesTheConnectionUp")
bool FFlockTravelCancelConnectingTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	FFlockRequestHandle Handle;
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served, FFlockTravelToSessionOptions(), &Handle);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	Handle.Cancel();
	TestTrue(TEXT("cancelled"), Travel->FailedAs(EFlockErrorType::Cancelled));
	TestEqual(TEXT("the connection still being made was given up"), F.Game->StoppedConnecting, 1);
	F.Game->Arrives(true);
	F.Frame();
	TestEqual(TEXT("answered once"), Travel->Times, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelCancelWaitingTest, "Travel.CancellingWhileWaitingForTheHostStopsTheWaitAndTravelsNowhere")
bool FFlockTravelCancelWaitingTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	FFlockRequestHandle Handle;
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served, FFlockTravelToSessionOptions(), &Handle);
	F.Advance(0.3);
	Handle.Cancel();
	TestTrue(TEXT("cancelled"), Travel->FailedAs(EFlockErrorType::Cancelled));
	TestEqual(TEXT("no wait left"), F.Sessions().GetConnectionWaitCountForTesting(), 0);
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.3);
	TestEqual(TEXT("no travel"), F.Game->TraveledTo.Num(), 0);
	TestFalse(TEXT("another travel may start"), F.Sessions().GetTravels().IsTravelingForTesting());
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelCancelOpeningTest, "Travel.CancellingWhileTheLevelOpensStopsItListeningOnceOpenAndPublishesNothing")
bool FFlockTravelCancelOpeningTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	FFlockRequestHandle Handle;
	const TSharedRef<FTravelAnswer> Travel = F.Travel(Session, FFlockTravelToSessionOptions(), &Handle);
	F.Frame();
	Handle.Cancel();
	TestTrue(TEXT("cancelled at once"), Travel->FailedAs(EFlockErrorType::Cancelled));
	const TSharedRef<FTravelAnswer> Meanwhile = F.Travel(Session);
	F.Frame();
	TestFalse(TEXT("a newer travel waits while the level opens"), Meanwhile->bAnswered);
	TestEqual(TEXT("opening nothing meanwhile"), F.Game->Opened.Num(), 1);
	F.Game->Opens(7777);
	F.FramesUntil([&]() { return Meanwhile->bAnswered; });
	TestEqual(TEXT("answered once"), Travel->Times, 1);
	// The newer travel decides once the level is open: it hosts on that level, which nobody stops or opens again.
	TestEqualSensitive(TEXT("the newer hosts"), Meanwhile->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	TestEqual(TEXT("on the level that opened, not stopped"), F.Game->StoppedListening, 0);
	TestEqual(TEXT("nor opened twice"), F.Game->Opened.Num(), 1);
	TestEqual(TEXT("published by the newer one alone"), F.Server->Count(ERoute::Publish), 1);
	// Cancelled with no newer travel, the level stops listening once open.
	FConnectingFixture G;
	G.SignIn(PlayerA);
	UFlockMultiplayerSession* Alone = G.Hosted(*this);
	if (Alone == nullptr)
	{
		return false;
	}
	FFlockRequestHandle AloneHandle;
	G.Travel(Alone, FFlockTravelToSessionOptions(), &AloneHandle);
	G.Frame();
	AloneHandle.Cancel();
	G.Game->Opens(7777);
	G.Frame();
	TestEqual(TEXT("a cancel alone stops it listening once open"), G.Game->StoppedListening, 1);
	TestEqual(TEXT("and publishes nothing"), G.Server->Count(ERoute::Publish), 0);
	return true;
}

// ---- Travel To Session again: a newer travel takes over, and a host change or a lost relay is this device's part done again ----

namespace FlockTravelRestartHelpers
{
	/** The session as Flock answers it now, read into the held one (its next heartbeat would bring it). */
	void ReadAgain(FConnectingFixture& F, UFlockMultiplayerSession* Session)
	{
		F.Provider->GetSession(Session->GetId(), [](TFlockResult<UFlockMultiplayerSession*>) {});
	}

	/** This game as B, connected directly to A in Level; the travel's answer. */
	TSharedRef<FTravelAnswer> ConnectedToA(FAutomationTestBase& Test, FConnectingFixture& F, FFlockFakeSessionServer::FSessionRef& OutServed,
		UFlockMultiplayerSession*& OutSession, const FString& Level = TEXT("/Game/Maps/Arena"))
	{
		OutSession = F.JoinedAsB(Test, OutServed);
		if (OutSession == nullptr)
		{
			return MakeShared<FTravelAnswer>();
		}
		const TSharedRef<FTravelAnswer> Travel = F.Travel(OutSession);
		F.Server->Publishes(OutServed, Direct(Text(HostsPublicAddress), Number(7777)));
		F.Advance(0.25);
		F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
		F.Game->Level = Level;
		F.Game->Arrives(true);
		F.Frame();
		Test.TestTrue(TEXT("precondition: connected to A"), Travel->Succeeded()
			&& Travel->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive));
		return Travel;
	}
}

using namespace FlockTravelRestartHelpers;

FLOCK_CONNECTING_TEST(FFlockTravelRestartFromAnswerTest, "Travel.Restart.ATravelStartedFromTheRestartedOnesAnswer_TakesOverInTurn")
bool FFlockTravelRestartFromAnswerTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	TSharedPtr<FTravelAnswer> Third;
	int32 FirstAnswers = 0;
	Session->TravelToSession(nullptr, FFlockTravelToSessionOptions(), [&F, Session, &Third, &FirstAnswers](TFlockResult<FFlockTravelToSessionResult>)
	{
		++FirstAnswers;
		// The game travels again from the restarted answer: that one takes over from the second in turn.
		Third = F.Travel(Session);
	});
	const TSharedRef<FTravelAnswer> Second = F.Travel(Session);
	TestEqual(TEXT("the first answered once"), FirstAnswers, 1);
	TestTrue(TEXT("the second, begun from nothing, is restarted at once by the one started from the first's answer"), Second->Succeeded()
		&& Second->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Restarted, ESearchCase::CaseSensitive));
	if (!TestTrue(TEXT("the third goes on"), Third.IsValid() && !Third->bAnswered))
	{
		return false;
	}
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.Frame();
	TestEqual(TEXT("one travel to the host in all"), F.Game->TraveledTo.Num(), 1);
	TestEqualSensitive(TEXT("the third connected"), Third->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Connected));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartSettleTest, "Travel.Restart.ATravelAskedWhileALevelOpens_BeginsOnceItHasOpened_OrIsRefused30sOn")
bool FFlockTravelRestartSettleTest::RunTest(const FString& Parameters)
{
	{
		FConnectingFixture F;
		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
		UFlockMultiplayerSession* Session = F.JoinedAsB(*this, Served);
		if (Session == nullptr)
		{
			return false;
		}
		// The engine's own way out of a lost connection, back to the default level, takes a frame.
		F.Game->State.bChangingLevel = true;
		const TSharedRef<FTravelAnswer> Travel = F.Travel(Session);
		F.Advance(29.0);
		TestFalse(TEXT("waits while the level opens"), Travel->bAnswered);
		TestEqual(TEXT("asking nothing meanwhile"), F.Sessions().GetConnectionWaitCountForTesting(), 0);
		F.Game->State.bChangingLevel = false;
		F.Frame();
		TestEqual(TEXT("then begins: waits for the host"), F.Sessions().GetConnectionWaitCountForTesting(), 1);
		F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
		F.Advance(0.25);
		F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
		TestEqual(TEXT("and travels to it"), F.Game->TraveledTo.Num(), 1);
	}
	FConnectingFixture G;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = G.JoinedAsB(*this, Served);
	if (Session == nullptr)
	{
		return false;
	}
	G.Game->State.bChangingLevel = true;
	const TSharedRef<FTravelAnswer> Stuck = G.Travel(Session);
	G.Advance(30.0);
	TestTrue(TEXT("a level that never finishes opening: refused 30 s on, saying so"), Stuck->FailedAs(EFlockErrorType::Validation)
		&& Stuck->Result.Error.Message.Contains(TEXT("still opening a level")));
	TestEqual(TEXT("the engine was asked nothing"), G.Game->TraveledTo.Num() + G.Game->Opened.Num(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartSettleConnectTest, "Travel.Restart.APlayerWhoseEngineChangesLevelAsItConnects_WaitsForIt_ThenTravels")
bool FFlockTravelRestartSettleConnectTest::RunTest(const FString& Parameters)
{
	{
		FConnectingFixture F;
		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
		const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
		F.Advance(0.5);
		// The engine's own way out of a lost connection takes a frame: it is under way as the host's address comes.
		F.Game->State.bChangingLevel = true;
		F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
		F.Advance(0.25);
		F.FramesUntil([&]() { return Travel->bAnswered || F.Game->TraveledTo.Num() > 0; });
		TestFalse(TEXT("before asking for its token: waits for the engine, not refused"), Travel->bAnswered);
		TestEqual(TEXT("asking nothing meanwhile"), F.Server->Count(ERoute::JoinToken), 0);
		F.Game->State.bChangingLevel = false;
		F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
		TestEqual(TEXT("then travels"), F.Game->TraveledTo.Num(), 1);
	}
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	const TSharedRef<FFlockFakePartyServer::FHeld> Token = F.Server->HoldNext(ERoute::JoinToken);
	F.Server->Publishes(Served, Direct(Text(HostsPublicAddress), Number(7777)));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Server->Count(ERoute::JoinToken) > 0; });
	// The token comes while the engine is on its way out of a lost connection.
	F.Game->State.bChangingLevel = true;
	Token->Release();
	F.FramesUntil([&]() { return Travel->bAnswered || F.Game->TraveledTo.Num() > 0; });
	TestFalse(TEXT("with its token: waits for the engine, not refused"), Travel->bAnswered);
	F.Game->State.bChangingLevel = false;
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqual(TEXT("then travels with it"), F.Game->TraveledTo.Num(), 1);
	TestFalse(TEXT("its token in the URL"), F.Game->TokensTraveledWith.Num() == 0 || F.Game->TokensTraveledWith[0].IsEmpty());
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartPlayerTest, "Travel.Restart.APlayerWithItsOldHost_WaitsThereForTheNewHost_AndConnectsWithANewToken")
bool FFlockTravelRestartPlayerTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> First = ConnectedToA(*this, F, Served, Session);
	if (Session == nullptr || !First->Succeeded())
	{
		return false;
	}
	F.Server->Joins(Served, PlayerC);
	// A hands hosting to C, and the address goes with it.
	F.Server->MakesHost(Served, PlayerC);
	ReadAgain(F, Session);
	if (!TestEqualSensitive(TEXT("precondition: C hosts"), Session->GetHostPlayerId(), PlayerC) || !TestFalse(TEXT("precondition: no address"), Session->HasConnection()))
	{
		return false;
	}
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	F.Advance(0.5);
	TestFalse(TEXT("waits for the new host, in the old host's level"), Again->bAnswered);
	TestEqual(TEXT("nothing opened or stopped meanwhile"), F.Game->Opened.Num() + F.Game->StoppedListening + F.Game->StoppedConnecting, 0);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.9")), Number(7777), TEXT("192.168.1.30"), TEXT("198.51.100.9")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 1; });
	if (!TestEqual(TEXT("traveled again, from the old host's level"), F.Game->TraveledTo.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("to the new host"), F.Game->TraveledTo.Last(), FString(TEXT("198.51.100.9:7777")));
	TestFalse(TEXT("with a new token"), F.Game->TokensTraveledWith.Last().IsEmpty() || F.Game->TokensTraveledWith.Last() == F.Game->TokensTraveledWith[0]);
	F.Game->Arrives(true);
	F.Frame();
	TestEqualSensitive(TEXT("connected"), Again->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Connected));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartNewHostTest, "Travel.Restart.APlayerThatBecameTheHost_OpensTheLevelItPlayed_EvenFromItsDefaultLevel")
bool FFlockTravelRestartNewHostTest::RunTest(const FString& Parameters)
{
	{
		FConnectingFixture F;
		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
		UFlockMultiplayerSession* Session = nullptr;
		if (!ConnectedToA(*this, F, Served, Session)->Succeeded())
		{
			return false;
		}
		// A changes level, B following it; then A leaves: B, seated longest of the others, hosts. The engine has taken B back to its
		// default level meanwhile.
		F.Game->Level = TEXT("/Game/Maps/Second");
		F.Frame();
		F.Server->Leaves(Served, PlayerA);
		F.Game->GoesBackToItsDefaultLevel();
		F.Game->Level = TEXT("/Game/Maps/Entry");
		F.Frame();
		ReadAgain(F, Session);
		if (!TestTrue(TEXT("precondition: B hosts"), Session->IsHost()))
		{
			return false;
		}
		const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
		TestEqualSensitive(TEXT("the level it played with A last, not the default one it is in"), FString::Join(F.Game->Opened, TEXT(",")),
			FString(TEXT("/Game/Maps/Second")));
		F.Game->Opens(7777);
		F.FramesUntil([&]() { return Again->bAnswered; });
		TestEqualSensitive(TEXT("hosts"), Again->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	}
	{
		FConnectingFixture F;
		FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
		UFlockMultiplayerSession* Session = nullptr;
		if (!ConnectedToA(*this, F, Served, Session)->Succeeded())
		{
			return false;
		}
		// Still in A's level, connected: the level opens from there, the engine's own connection going with it.
		F.Server->Leaves(Served, PlayerA);
		ReadAgain(F, Session);
		F.Travel(Session);
		TestEqualSensitive(TEXT("from the old host's level, that level"), FString::Join(F.Game->Opened, TEXT(",")), FString(TEXT("/Game/Maps/Arena")));
		TestEqual(TEXT("nothing stopped first"), F.Game->StoppedListening + F.Game->StoppedConnecting, 0);
	}
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = nullptr;
	if (!ConnectedToA(*this, F, Served, Session)->Succeeded())
	{
		return false;
	}
	F.Server->Leaves(Served, PlayerA);
	F.Game->GoesBackToItsDefaultLevel();
	F.Game->Level = TEXT("/Game/Maps/Entry");
	F.Frame();
	ReadAgain(F, Session);
	FFlockTravelToSessionOptions Named;
	Named.HostLevel = TEXT("/Game/Maps/Other");
	F.Travel(Session, Named);
	TestEqualSensitive(TEXT("a level the game names wins"), FString::Join(F.Game->Opened, TEXT(",")), FString(TEXT("/Game/Maps/Other")));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartRoleTest, "Travel.Restart.AHostChangeReadWhileTheEngineSettles_IsTaken")
bool FFlockTravelRestartRoleTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = nullptr;
	if (!ConnectedToA(*this, F, Served, Session)->Succeeded())
	{
		return false;
	}
	// The engine takes B back to its default level (A went), and B travels again while it does, still a player.
	F.Game->State.bChangingLevel = true;
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	F.Server->Leaves(Served, PlayerA);
	ReadAgain(F, Session);
	if (!TestTrue(TEXT("precondition: B hosts now"), Session->IsHost()))
	{
		return false;
	}
	F.Game->GoesBackToItsDefaultLevel();
	F.Frame();
	TestEqual(TEXT("it hosts, as the session says once the engine settled"), F.Game->Opened.Num(), 1);
	TestEqual(TEXT("waiting for no host"), F.Sessions().GetConnectionWaitCountForTesting(), 0);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartSameHostTest, "Travel.Restart.APlayerInTheGameOfTheHostTheSessionNames_TravelingAgain_IsConnectedAtOnce")
bool FFlockTravelRestartSameHostTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> First = ConnectedToA(*this, F, Served, Session);
	if (!First->Succeeded())
	{
		return false;
	}
	const int32 Tokens = F.Server->Count(ERoute::JoinToken);
	// Called again with nothing changed (a level's Begin Play runs on each arriving player, say).
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	TestTrue(TEXT("connected at once"), Again->Succeeded()
		&& Again->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Connected, ESearchCase::CaseSensitive));
	TestEqualSensitive(TEXT("with the host it is with"), Again->Result.Value.Address, First->Result.Value.Address);
	TestEqual(TEXT("traveling nowhere"), F.Game->TraveledTo.Num(), 1);
	TestEqual(TEXT("asking nothing"), F.Server->Count(ERoute::JoinToken), Tokens);
	TestTrue(TEXT("still with its host"), F.Sessions().GetTravels().IsWithTheHostForTesting());
	// Its token on hand is still kept, the stay with the host not started over.
	F.Advance(41.0);
	F.Frame();
	TestEqual(TEXT("its token on hand still renewed"), F.Server->Count(ERoute::JoinToken), Tokens + 1);
	// Once another player hosts, a travel is no longer answered at once: it waits for the new host.
	F.Server->Joins(Served, PlayerC);
	F.Server->MakesHost(Served, PlayerC);
	ReadAgain(F, Session);
	const TSharedRef<FTravelAnswer> ForTheNewHost = F.Travel(Session);
	TestFalse(TEXT("control: a host change makes it wait for the new host"), ForTheNewHost->bAnswered);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartOtherServerTest, "Travel.Restart.AGameNowOnAServerOfItsOwn_IsRefused_ThoughItPlayedTheSession")
bool FFlockTravelRestartOtherServerTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	UFlockMultiplayerSession* Session = nullptr;
	if (!ConnectedToA(*this, F, Served, Session)->Succeeded())
	{
		return false;
	}
	// The game leaves A for a server of its own, still holding its seat.
	F.Game->GoesBackToItsDefaultLevel();
	F.Frame();
	F.Game->TravelTo(TEXT("10.0.0.9:7777"));
	F.Game->Arrives(true);
	F.Frame();
	F.Server->Leaves(Served, PlayerA);
	ReadAgain(F, Session);
	const int32 Opened = F.Game->Opened.Num();
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	TestTrue(TEXT("refused: it is connected to another server"), Again->FailedAs(EFlockErrorType::Validation)
		&& Again->Result.Error.Message.Contains(TEXT("already connected")));
	TestEqual(TEXT("nothing opened over it"), F.Game->Opened.Num(), Opened);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartKeepHostingTest, "Travel.Restart.AHostListeningForTheSession_KeepsItsLevelAndItsPlayers_AndPublishesAgain")
bool FFlockTravelRestartKeepHostingTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> First = HostTravel(*this, F, Session);
	if (Session == nullptr)
	{
		return false;
	}
	F.Game->Opens(7777);
	F.FramesUntil([&]() { return First->bAnswered; });
	if (!TestTrue(TEXT("precondition: hosting"), First->Succeeded()))
	{
		return false;
	}
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	F.FramesUntil([&]() { return Again->bAnswered; });
	TestEqualSensitive(TEXT("hosts"), Again->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	TestEqual(TEXT("on the port it listens on"), Again->Result.Value.Port, 7777);
	TestEqual(TEXT("its level kept: nothing opened again"), F.Game->Opened.Num(), 1);
	TestEqual(TEXT("nor stopped: its players stay"), F.Game->StoppedListening, 0);
	TestEqual(TEXT("published again"), F.Server->Count(ERoute::Publish), 2);
	TestTrue(TEXT("still checking joining players"), F.Sessions().GetJoinChecks().IsHosting(Session));
	// Its publish refused: what it published before stands, and its players stay.
	F.Server->AnswerNext(ERoute::Publish, FFlockFakeSessionServer::Refused(503, TEXT("service.unavailable")));
	const TSharedRef<FTravelAnswer> Refused = F.Travel(Session);
	F.FramesUntil([&]() { return Refused->bAnswered; });
	TestTrue(TEXT("a refused publish fails"), Refused->bAnswered && !Refused->Result.bSuccess);
	TestEqual(TEXT("and leaves it listening"), F.Game->StoppedListening, 0);
	// Asked again while it publishes, the older ends restarted and still nothing stops.
	const TSharedRef<FFlockFakePartyServer::FHeld> Held = F.Server->HoldNext(ERoute::Publish);
	const TSharedRef<FTravelAnswer> Older = F.Travel(Session);
	F.Frame();
	const TSharedRef<FTravelAnswer> Newer = F.Travel(Session);
	TestTrue(TEXT("the older restarted"), Older->Succeeded() && Older->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Restarted, ESearchCase::CaseSensitive));
	Held->Release();
	F.FramesUntil([&]() { return Newer->bAnswered; });
	TestEqualSensitive(TEXT("the newer hosts on"), Newer->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	TestEqual(TEXT("its level never stopped"), F.Game->StoppedListening, 0);
	TestEqual(TEXT("nor opened again"), F.Game->Opened.Num(), 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartWhileOpeningTest, "Travel.Restart.AHostAskedAgainWhileItsLevelOpensOrItPublishes_KeepsTheLevel")
bool FFlockTravelRestartWhileOpeningTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> First = HostTravel(*this, F, Session);
	if (Session == nullptr)
	{
		return false;
	}
	F.Frame();
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	TestTrue(TEXT("the first restarted"), First->Succeeded() && First->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Restarted, ESearchCase::CaseSensitive));
	F.Game->Opens(7777);
	F.FramesUntil([&]() { return Again->bAnswered; });
	TestEqualSensitive(TEXT("the newer hosts"), Again->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	TestEqual(TEXT("on the level the first opened, never stopped"), F.Game->StoppedListening, 0);
	TestEqual(TEXT("nor opened twice"), F.Game->Opened.Num(), 1);
	// The level it opened is open and the travel publishes: asked again, the level stays, for the newer one to host on.
	FConnectingFixture G;
	UFlockMultiplayerSession* Publishing = nullptr;
	const TSharedRef<FTravelAnswer> Opener = HostTravel(*this, G, Publishing);
	if (Publishing == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Held = G.Server->HoldNext(ERoute::Publish);
	G.Game->Opens(7777);
	G.FramesUntil([&]() { return G.Server->Count(ERoute::Publish) > 0; });
	const TSharedRef<FTravelAnswer> Newer = G.Travel(Publishing);
	TestTrue(TEXT("the opener restarted while it published"), Opener->Succeeded()
		&& Opener->Result.Value.Outcome.Equals(FlockTravelToSessionOutcomes::Restarted, ESearchCase::CaseSensitive));
	Held->Release();
	G.FramesUntil([&]() { return Newer->bAnswered; });
	TestEqualSensitive(TEXT("the newer hosts on"), Newer->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::StartedAsHost));
	TestEqual(TEXT("on the level the opener opened, never stopped"), G.Game->StoppedListening, 0);
	TestEqual(TEXT("nor opened again"), G.Game->Opened.Num(), 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelRestartHostToPlayerTest, "Travel.Restart.AHostThatHandedHostingOver_StopsListening_AndJoinsTheNewHost")
bool FFlockTravelRestartHostToPlayerTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	UFlockMultiplayerSession* Session = nullptr;
	const TSharedRef<FTravelAnswer> First = HostTravel(*this, F, Session);
	if (Session == nullptr)
	{
		return false;
	}
	F.Game->Opens(7777);
	F.FramesUntil([&]() { return First->bAnswered; });
	const FFlockFakeSessionServer::FSessionRef Served = F.Server->ById(Session->GetId()).ToSharedRef();
	F.Server->Joins(Served, PlayerB);
	F.Server->MakesHost(Served, PlayerB);
	ReadAgain(F, Session);
	if (!TestFalse(TEXT("precondition: B hosts now"), Session->IsHost()))
	{
		return false;
	}
	const TSharedRef<FTravelAnswer> Again = F.Travel(Session);
	TestEqual(TEXT("stopped listening at once: nobody joins it any more"), F.Game->StoppedListening, 1);
	F.Frame();
	TestFalse(TEXT("its check of joining players ended with it"), F.Sessions().GetJoinChecks().IsHosting(Session));
	TestFalse(TEXT("waits for the new host"), Again->bAnswered);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.9")), Number(7777), TEXT("192.168.1.30"), TEXT("198.51.100.9")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	TestEqualSensitive(TEXT("and travels to it"), FString::Join(F.Game->TraveledTo, TEXT(",")), FString(TEXT("198.51.100.9:7777")));
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelShutdownTest, "Travel.FlockShuttingDownEndsATravelAsCancelledAndLeavesTheEngineAlone")
bool FFlockTravelShutdownTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Provider->Shutdown();
	TestTrue(TEXT("cancelled"), Travel->FailedAs(EFlockErrorType::Cancelled));
	TestEqual(TEXT("the engine left as it is"), F.Game->StoppedConnecting + F.Game->StoppedListening, 0);
	F.Game->Fails(TEXT("ConnectionTimeout: late"));
	TestEqual(TEXT("answered once"), Travel->Times, 1);
	return true;
}

FLOCK_CONNECTING_TEST(FFlockTravelEarlierFailureTest, "Travel.AFailureToldBeforeTheConnectionBeganDoesNotEndIt")
bool FFlockTravelEarlierFailureTest::RunTest(const FString& Parameters)
{
	FConnectingFixture F;
	FFlockFakeSessionServer::FSessionRef Served = MakeShared<FFlockFakeSessionServer::FSession>();
	const TSharedRef<FTravelAnswer> Travel = PlayerTravel(*this, F, Served);
	F.Game->Fails(TEXT("ConnectionLost: an earlier connection of the game's"));
	F.Server->Publishes(Served, Direct(Text(TEXT("198.51.100.4")), Number(7777), TEXT("192.168.1.20"), TEXT("198.51.100.4")));
	F.Advance(0.25);
	F.FramesUntil([&]() { return F.Game->TraveledTo.Num() > 0; });
	F.Game->Arrives(true);
	F.Frame();
	TestEqualSensitive(TEXT("connected"), Travel->Result.Value.Outcome, FString(FlockTravelToSessionOutcomes::Connected));
	return true;
}

// ---- the engine's own side, where it runs ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockTravelEngineSideTest, "Flock.Multiplayer.Connecting.Travel.TheEngineSideReadsTheRunningGamesWorldAndTheBoundPort",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FFlockTravelEngineSideTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game && Context.World() != nullptr)
		{
			World = Context.World();
		}
	}
	FString Refusal;
	TestFalse(TEXT("no object, no world: refused"), FlockEngineTravel()(nullptr, Refusal).IsValid());
	TestTrue(TEXT("said why"), Refusal.Contains(TEXT("world")));
	if (World == nullptr)
	{
		AddInfo(TEXT("No game world here (the editor's pass): the engine side runs under -game."));
		return true;
	}
	const TSharedPtr<IFlockGameTravel> Game = FlockEngineTravel()(World, Refusal);
	if (!TestTrue(TEXT("the running game's engine side"), Game.IsValid()))
	{
		return false;
	}
	// The engine's failures this game's travel hears: its own world's, not one with no world and no connection of its own.
	// Told through the engine's delegate alone (no driver given, so the engine's own handler only logs it).
	TArray<FString> Heard;
	Game->WatchFailures([&Heard](const FString& Kind, const FString& Words) { Heard.Add(Kind + TEXT(": ") + Words); });
	GEngine->OnNetworkFailure().Broadcast(nullptr, nullptr, ENetworkFailure::ConnectionLost, TEXT("another game's"));
	GEngine->OnNetworkFailure().Broadcast(World, nullptr, ENetworkFailure::ConnectionLost, TEXT("this game's world"));
	TestEqual(TEXT("one failure heard"), Heard.Num(), 1);
	TestTrue(TEXT("its own world's, with the engine's words"), Heard.Num() == 1 && Heard[0].Contains(TEXT("this game's world"))
		&& Heard[0].Contains(TEXT("ConnectionLost")));

	Game->MarkWorld();
	const FFlockGameNetState Before = Game->Read();
	TestTrue(TEXT("a world"), Before.bHasWorld);
	TestFalse(TEXT("the world marked"), Before.bInAnotherWorld);
	TestFalse(TEXT("no level opening"), Before.bChangingLevel);
	TestEqual(TEXT("on its own"), static_cast<int32>(Before.NetMode), static_cast<int32>(NM_Standalone));
	TestFalse(TEXT("the level it is in"), Game->CurrentLevel().IsEmpty());

	// Listening in place (the engine's own call, measured to take 2 ms) stands in for a level opened with ?listen.
	FURL Url(nullptr, *World->URL.ToString(), TRAVEL_Absolute);
	if (!TestTrue(TEXT("precondition: the world listens"), World->Listen(Url)))
	{
		return false;
	}
	const FFlockGameNetState Listening = Game->Read();
	const TSharedPtr<const FInternetAddr> Bound = World->GetNetDriver() != nullptr ? World->GetNetDriver()->GetLocalAddr() : nullptr;
	TestEqual(TEXT("a listen server"), static_cast<int32>(Listening.NetMode), static_cast<int32>(NM_ListenServer));
	TestTrue(TEXT("the port the net driver bound"), Bound.IsValid() && Listening.ListeningPort == Bound->GetPort() && Listening.ListeningPort > 0);
	TestTrue(TEXT("a host's world goes to no host"), Listening.Destination.IsEmpty());
	TestEqual(TEXT("a connection never handed out comes from no loopback port"), Game->LoopbackPortOf(42), 0);
	TestFalse(TEXT("nobody connects from a loopback port yet"), Game->IsAnyConnectionFromLoopbackPort(Listening.ListeningPort));
	// A connection being made is the engine's own pending connection: a join token goes into its URL's options before its login.
	FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	if (Context != nullptr && Context->PendingNetGame == nullptr)
	{
		TestFalse(TEXT("no connection being made: no token put"), Game->PutJoinTokenOnTheConnectionBeingMade(TEXT("none")));
		UPendingNetGame* Pending = NewObject<UPendingNetGame>();
		Pending->Initialize(FURL(nullptr, TEXT("127.0.0.1:9/Engine/Maps/Entry?flockjointoken=first?Name=Ann"), TRAVEL_Absolute));
		Context->PendingNetGame = Pending;
		const FFlockGameNetState Connecting = Game->Read();
		TestTrue(TEXT("read as a connection being made"), Connecting.bConnecting);
		TestEqualSensitive(TEXT("to where it goes"), Connecting.Destination, FString(TEXT("127.0.0.1:9")));
		TestTrue(TEXT("a token put on it"), Game->PutJoinTokenOnTheConnectionBeingMade(TEXT("second")));
		const TArray<FString> Tokens = Pending->URL.Op.FilterByPredicate([](const FString& Option) { return Option.StartsWith(TEXT("FlockJoinToken="), ESearchCase::IgnoreCase); });
		TestTrue(TEXT("in the first one's place, whatever its letters"), Tokens.Num() == 1 && Tokens[0].Equals(TEXT("FlockJoinToken=second"), ESearchCase::CaseSensitive));
		TestEqualSensitive(TEXT("the rest kept"), FString(Pending->URL.GetOption(TEXT("Name="), TEXT(""))), FString(TEXT("Ann")));
		// A connection the engine makes (its own driver, toward a port nobody answers), and the host's engine answering it with its
		// challenge: the login has gone with the URL as it was, so nothing is put. Given up as the engine gives up a connection.
		Pending->InitNetDriver();
		UNetConnection* ToTheHost = Pending->NetDriver != nullptr ? Pending->NetDriver->ServerConnection.Get() : nullptr;
		if (TestNotNull(TEXT("precondition: the engine's connection being made"), ToTheHost))
		{
			TestFalse(TEXT("not answered yet"), Game->Read().bHostAnswered);
			TestTrue(TEXT("a token put before the host's engine answers"), Game->PutJoinTokenOnTheConnectionBeingMade(TEXT("second")));
			ToTheHost->Challenge = TEXT("the host's challenge");
			TestTrue(TEXT("read as answered by the host's engine"), Game->Read().bHostAnswered);
			TestFalse(TEXT("a token once the login has gone is put nowhere"), Game->PutJoinTokenOnTheConnectionBeingMade(TEXT("third")));
			TestEqualSensitive(TEXT("the login's token stays"), FString(Pending->URL.GetOption(TEXT("FlockJoinToken="), TEXT(""))), FString(TEXT("second")));
		}
		GEngine->CancelPending(World);
		TestFalse(TEXT("on its own again: no connection being made"), Game->Read().bConnecting);
	}
	Game->StopListening();
	const FFlockGameNetState After = Game->Read();
	TestEqual(TEXT("on its own again"), static_cast<int32>(After.NetMode), static_cast<int32>(NM_Standalone));
	TestEqual(TEXT("listening on nothing"), After.ListeningPort, 0);
	return true;
}

#endif
