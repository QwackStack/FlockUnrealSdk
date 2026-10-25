// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Multiplayer/FlockSockets.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Multiplayer/Relay/FlockRelayStream.h"
#include "Multiplayer/Relay/FlockRelayThreads.h"
#include "Multiplayer/Relay/FlockRelayTls.h"
#include "Multiplayer/Relay/FlockRelayWays.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/Support/FlockFakeRelayServer.h"
#include "Tests/Support/FlockRecordingLogger.h"

// Relay connections over a fake relay on loopback (FFlockFakeRelayServer), for the relay client's and the relay bridge's tests.
// Include only from guarded test files.

namespace FlockRelayTestHelpers
{
	const FString RelayPlayerA = TEXT("player-a");
	const FString RelayPlayerB = TEXT("player-b");
	const FString OtherRelayIp = TEXT("203.0.113.51");

	using FOpenedResult = TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>;

	struct FOpening
	{
		bool bAnswered = false;
		int32 Times = 0;
		FOpenedResult Result;
		uint32 AnsweredOnThread = 0;
	};

	struct FOpenedTo
	{
		bool bAnswered = false;
		TFlockRelayResult<bool> Result;
	};

	inline TFunction<void(FOpenedResult)> Into(const TSharedRef<FOpening>& Answer)
	{
		return [Answer](FOpenedResult Result)
		{
			Answer->bAnswered = true;
			++Answer->Times;
			Answer->Result = Result;
			Answer->AnsweredOnThread = FPlatformTLS::GetCurrentThreadId();
		};
	}

	inline FFlockRelayTiming Quick()
	{
		FFlockRelayTiming Timing;
		Timing.AnswerWaitSeconds = 2.0;
		Timing.NameLookUpWaitSeconds = 1.0;
		return Timing;
	}

	/** Stands in for the system's lookup: every name is on this machine's loopback, 127.0.0.1, and ::1 too when bWithIpv6. */
	inline FFlockNameLookup::FLookUp EveryNameOnLoopback(bool bWithIpv6 = false)
	{
		return [bWithIpv6](const FString&, TFunction<void(const TArray<FString>&)> Answer)
		{
			TArray<FString> Addresses = { TEXT("127.0.0.1") };
			if (bWithIpv6)
			{
				Addresses.Add(TEXT("::1"));
			}
			Answer(Addresses);
		};
	}

	/** As a network with IPv6 alone answers a name: a native or translated IPv6 address and no IPv4 one. */
	inline FFlockNameLookup::FLookUp EveryNameOnIpv6Loopback()
	{
		return [](const FString&, TFunction<void(const TArray<FString>&)> Answer) { Answer({ TEXT("::1") }); };
	}

	inline FFlockRelayPeer Peer(const FString& Text)
	{
		FFlockRelayPeer Out;
		FFlockRelayPeer::TryParse(Text, Out);
		return Out;
	}

	inline TArray<uint8> Bytes(const FString& Text) { return FFlockRelayWire::Utf8(Text); }

	/** A UDP socket on 127.0.0.1 standing in for the engine's net driver. */
	struct FEngineSocket
	{
		FSocket* Socket = FFlockSockets::OpenOnLoopback(TEXT("Flock test engine"));
		int32 Port = Socket != nullptr ? FFlockSockets::LocalPortOf(*Socket) : 0;
		/** Everything it heard so far, and the port each came from. */
		TArray<TPair<FString, int32>> Heard;

		FEngineSocket() = default;
		FEngineSocket(const FEngineSocket&) = delete;
		~FEngineSocket() { FFlockSockets::Close(Socket); }

		bool SendTo(int32 ToPort, const TArray<uint8>& Bytes) const
		{
			int32 Sent = 0;
			return Socket != nullptr && Socket->SendTo(Bytes.GetData(), Bytes.Num(), Sent, *FFlockSockets::OnLoopback(ToPort));
		}

		bool SendTo(int32 ToPort, const FString& Text) const { return SendTo(ToPort, FFlockRelayWire::Utf8(Text)); }

		void ReadEverything()
		{
			uint8 Buffer[2048];
			int32 Read = 0;
			const TSharedRef<FInternetAddr> From = FFlockSockets::OnLoopback(0);
			while (Socket != nullptr && Socket->RecvFrom(Buffer, UE_ARRAY_COUNT(Buffer), Read, *From))
			{
				Heard.Emplace(FFlockRelayWire::FromUtf8(Buffer, Read), From->GetPort());
			}
		}

		/** The port Text came from, or 0 when it has not been heard. */
		int32 PortOf(const FString& Text) const
		{
			const TPair<FString, int32>* Found = Heard.FindByPredicate([&Text](const TPair<FString, int32>& Each) { return Each.Key == Text; });
			return Found != nullptr ? Found->Value : 0;
		}
	};

	/** Relay connections over a fake relay on loopback (taking TCP and TLS too when made with streams); each is closed when the test ends. */
	struct FRelayFixture
	{
		TUniquePtr<FFlockFakeRelayServer> Relay;
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TArray<TSharedRef<FFlockRelayConnection>> Connections;
		TArray<TSharedRef<FFlockRelayWays>> Races;
		/** The connection each race kept, ticked with every frame and closed when the test ends. */
		TSharedRef<TArray<TSharedRef<FFlockRelayConnection>>> Kept = MakeShared<TArray<TSharedRef<FFlockRelayConnection>>>();
		/** The STUN servers each relay opened from now on lists with its login, as Flock lists them. */
		TArray<FFlockRelayStunLookUp> StunServers;

		explicit FRelayFixture(bool bWithStreams = false)
			: Relay(MakeUnique<FFlockFakeRelayServer>(bWithStreams))
		{
			FFlockSockets::ResetForTesting();
		}

		~FRelayFixture()
		{
			for (const TSharedRef<FFlockRelayWays>& Race : Races)
			{
				Race->Close();
			}
			Races.Reset();
			for (const TSharedRef<FFlockRelayConnection>& Connection : *Kept)
			{
				Connection->Close();
			}
			Kept->Reset();
			for (const TSharedRef<FFlockRelayConnection>& Connection : Connections)
			{
				Connection->Close();
			}
			Connections.Reset();
			FFlockRelayConnection::MostOpeningsForTesting = 0;
			FFlockRelayThreads::bRefusedForTesting = false;
			FFlockRelayWays::AfterAWayOpenedForTesting = nullptr;
			FFlockRelayTls::SetTrustedCertificateForTesting(FString());
			FFlockRelayStream::bHoldsSendsForTesting = false;
			FFlockSockets::ResetForTesting();
		}

		/** A way in to the fake relay over Transport, at its port for that transport. */
		FFlockRelayLogin Login(const FString& Player, const FString& Host = TEXT("127.0.0.1"), EFlockRelayTransport Transport = EFlockRelayTransport::Udp) const
		{
			FFlockRelayLogin Made;
			Made.Host = Host;
			Made.Port = Transport == EFlockRelayTransport::Tls ? Relay->GetTlsPort() : Transport == EFlockRelayTransport::Tcp ? Relay->GetTcpPort() : Relay->GetPort();
			Made.Username = TEXT("1760000000:game:") + Player;
			Made.Password = TEXT("minted-for-the-test");
			Made.Transport = Transport;
			return Made;
		}

		/** Races Logins as a session does; the race is ticked with every frame and closed when the test ends. */
		TSharedRef<FOpening> Race(const TArray<FFlockRelayLogin>& Logins, const FFlockRelayTiming& Timing = Quick())
		{
			const TSharedRef<FOpening> Answer = MakeShared<FOpening>();
			const TSharedRef<FOpening> Into = Answer;
			const TSharedRef<TArray<TSharedRef<FFlockRelayConnection>>> KeptHere = Kept;
			Races.Add(FFlockRelayWays::Start(Logins, StunServers, Timing, Log, [KeptHere, Into](FOpenedResult Result)
			{
				if (Result.bSuccess && Result.Value.IsValid())
				{
					KeptHere->Add(Result.Value.ToSharedRef());
				}
				Into->bAnswered = true;
				++Into->Times;
				Into->Result = Result;
				Into->AnsweredOnThread = FPlatformTLS::GetCurrentThreadId();
			}));
			return Answer;
		}

		TSharedRef<FOpening> Open(const FFlockRelayLogin& WithLogin, const FFlockRelayTiming& Timing = Quick())
		{
			const TSharedRef<FOpening> Answer = MakeShared<FOpening>();
			Connections.Add(FFlockRelayConnection::Open(WithLogin, StunServers, Timing, Log, Into(Answer)));
			return Answer;
		}

		TSharedRef<FOpening> Open(const FString& Player, const FFlockRelayTiming& Timing = Quick()) { return Open(Login(Player), Timing); }

		/** One frame: every race and every connection hands the game what it has. */
		void Frame()
		{
			const TArray<TSharedRef<FFlockRelayWays>> EachRace = Races;
			for (const TSharedRef<FFlockRelayWays>& Racing : EachRace)
			{
				Racing->Tick();
			}
			TArray<TSharedRef<FFlockRelayConnection>> Each = Connections;
			Each.Append(*Kept);
			for (const TSharedRef<FFlockRelayConnection>& Connection : Each)
			{
				Connection->Tick();
			}
		}

		/** Runs frames, a millisecond apart, until Done or Seconds pass. */
		bool Until(TFunctionRef<bool()> Done, double Seconds = 10.0)
		{
			const double Ends = FPlatformTime::Seconds() + Seconds;
			while (!Done() && FPlatformTime::Seconds() < Ends)
			{
				Frame();
				FPlatformProcess::Sleep(0.001f);
			}
			Frame();
			return Done();
		}

		void RealSeconds(double Seconds) { Until([]() { return false; }, Seconds); }

		TSharedPtr<FFlockRelayConnection> Opened(FAutomationTestBase& Test, const FString& Player, const FFlockRelayTiming& Timing = Quick())
		{
			const TSharedRef<FOpening> Answer = Open(Player, Timing);
			Until([&Answer]() { return Answer->bAnswered; });
			Test.TestTrue(FString::Printf(TEXT("precondition: %s's relay opened (%s)"), *Player, *Answer->Result.Failure.Error.Message), Answer->Result.bSuccess);
			return Answer->Result.bSuccess ? Answer->Result.Value : nullptr;
		}

		TSharedRef<FOpenedTo> OpenTo(const TSharedPtr<FFlockRelayConnection>& Connection, const FFlockRelayPeer& To, FFlockRequestHandle* OutHandle = nullptr)
		{
			const TSharedRef<FOpenedTo> Answer = MakeShared<FOpenedTo>();
			const FFlockRequestHandle Handle = Connection->OpenTo(To, [Answer](TFlockRelayResult<bool> Result)
			{
				Answer->bAnswered = true;
				Answer->Result = Result;
			});
			if (OutHandle != nullptr)
			{
				*OutHandle = Handle;
			}
			return Answer;
		}

		/** A host and a player, each with a relay address, both opened to the host's. */
		bool HostAndPlayer(FAutomationTestBase& Test, TSharedPtr<FFlockRelayConnection>& OutHost, TSharedPtr<FFlockRelayConnection>& OutPlayer,
			const FFlockRelayTiming& Timing = Quick())
		{
			OutHost = Opened(Test, RelayPlayerA, Timing);
			OutPlayer = Opened(Test, RelayPlayerB, Timing);
			if (!OutHost.IsValid() || !OutPlayer.IsValid())
			{
				return false;
			}
			const TSharedRef<FOpenedTo> HostOpen = OpenTo(OutHost, OutHost->GetAddress());
			const TSharedRef<FOpenedTo> PlayerOpen = OpenTo(OutPlayer, OutHost->GetAddress());
			Until([&]() { return HostOpen->bAnswered && PlayerOpen->bAnswered; });
			return Test.TestTrue(TEXT("precondition: both opened to the host's relay address"), HostOpen->Result.bSuccess && PlayerOpen->Result.bSuccess);
		}

		/** Sends Text and waits for it at Receiver; where it was heard from. */
		bool Delivered(FAutomationTestBase& Test, const TSharedPtr<FFlockRelayConnection>& From, const FFlockRelayPeer& To,
			const TSharedPtr<FFlockRelayConnection>& Receiver, const FString& Text, FFlockRelayPeer* OutHeardFrom = nullptr, double Seconds = 3.0)
		{
			const TArray<uint8> Payload = Bytes(Text);
			if (!Test.TestTrue(FString::Printf(TEXT("sent \"%s\""), *Text), From->Send(To, Payload.GetData(), Payload.Num())))
			{
				return false;
			}
			uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
			bool bHeard = false;
			Until([&]()
			{
				int32 Count = 0;
				FFlockRelayPeer HeardFrom;
				while (!bHeard && Receiver->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, HeardFrom))
				{
					if (FFlockRelayWire::FromUtf8(Buffer, Count) == Text)
					{
						bHeard = true;
						if (OutHeardFrom != nullptr)
						{
							*OutHeardFrom = HeardFrom;
						}
					}
				}
				return bHeard;
			}, Seconds);
			return Test.TestTrue(FString::Printf(TEXT("\"%s\" arrived within %.0f s"), *Text, Seconds), bHeard);
		}

		/** Whether Text arrives at Receiver within Seconds; nothing is asserted. */
		bool Arrives(const TSharedPtr<FFlockRelayConnection>& Receiver, const FString& Text, double Seconds)
		{
			uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
			bool bHeard = false;
			Until([&]()
			{
				int32 Count = 0;
				FFlockRelayPeer HeardFrom;
				while (!bHeard && Receiver->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, HeardFrom))
				{
					bHeard = FFlockRelayWire::FromUtf8(Buffer, Count) == Text;
				}
				return bHeard;
			}, Seconds);
			return bHeard;
		}

		int32 Signed(uint16 Method) const
		{
			return Relay->SeenRequests(Method).FilterByPredicate([](const FFlockFakeRelayServer::FSeen& Seen) { return Seen.bSigned; }).Num();
		}
	};

	/** The protocol this machine gives a dual-stack socket: IPv6 where it has one, as on the desktops this runs on. */
	inline FName ProtocolThisMachineGivesARelaySocket()
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
