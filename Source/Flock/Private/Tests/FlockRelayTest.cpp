// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "HAL/MemoryBase.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/FlockEngineCompat.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Multiplayer/Relay/FlockRelayRequests.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Tests/Support/FlockConnectingFixture.h"
#include "Tests/Support/FlockFakeRelayServer.h"
#include "Tests/Support/FlockRecordingLogger.h"
#include "Tests/Support/FlockSessionTestListener.h"
#include "Tests/Support/FlockTestRetries.h"
#include "Tests/Support/FlockTestSpelling.h"

// The relay client against a TURN relay in memory on loopback (FFlockFakeRelayServer, coturn's answers as measured): reserving
// an address, opening it to other relay addresses, packets both ways, renewals, each failure on its own reason, reaching a relay
// that answers as Cloudflare's did (only some ports, an IP for each address), and a session's relay given back when it ends.

#define FLOCK_RELAY_TEST(TestClass, Name) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(TestClass, "Flock.Multiplayer.Relay." Name, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

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

	inline FFlockRelayPeer Peer(const FString& Text)
	{
		FFlockRelayPeer Out;
		FFlockRelayPeer::TryParse(Text, Out);
		return Out;
	}

	inline TArray<uint8> Bytes(const FString& Text) { return FFlockRelayWire::Utf8(Text); }

	/** Counts the game thread's heap allocations while installed, passing every call on to the allocator it replaced. */
	class FCountingAllocator : public FMalloc
	{
	public:
		explicit FCountingAllocator(FMalloc* InInner) : Inner(InInner) {}

		std::atomic<int32> GameThreadAllocations{ 0 };

		virtual void* Malloc(SIZE_T Count, uint32 Alignment) override { CountIfOnTheGameThread(); return Inner->Malloc(Count, Alignment); }
		virtual void* TryMalloc(SIZE_T Count, uint32 Alignment) override { CountIfOnTheGameThread(); return Inner->TryMalloc(Count, Alignment); }
		virtual void* Realloc(void* Original, SIZE_T Count, uint32 Alignment) override { CountIfOnTheGameThread(); return Inner->Realloc(Original, Count, Alignment); }
		virtual void* TryRealloc(void* Original, SIZE_T Count, uint32 Alignment) override { CountIfOnTheGameThread(); return Inner->TryRealloc(Original, Count, Alignment); }
		virtual void Free(void* Original) override { Inner->Free(Original); }
		virtual SIZE_T QuantizeSize(SIZE_T Count, uint32 Alignment) override { return Inner->QuantizeSize(Count, Alignment); }
		virtual bool GetAllocationSize(void* Original, SIZE_T& SizeOut) override { return Inner->GetAllocationSize(Original, SizeOut); }
		virtual void Trim(bool bTrimThreadCaches) override { Inner->Trim(bTrimThreadCaches); }
		virtual void SetupTLSCachesOnCurrentThread() override { Inner->SetupTLSCachesOnCurrentThread(); }
		virtual void ClearAndDisableTLSCachesOnCurrentThread() override { Inner->ClearAndDisableTLSCachesOnCurrentThread(); }
		virtual bool IsInternallyThreadSafe() const override { return Inner->IsInternallyThreadSafe(); }
		virtual const TCHAR* GetDescriptiveName() override { return TEXT("Flock test allocation counter"); }
		virtual void UpdateStats() override { Inner->UpdateStats(); }
		virtual void GetAllocatorStats(FGenericMemoryStats& OutStats) override { Inner->GetAllocatorStats(OutStats); }
		virtual bool ValidateHeap() override { return Inner->ValidateHeap(); }
		virtual void InitializeStatsMetadata() override { Inner->InitializeStatsMetadata(); }
		virtual void DumpAllocatorStats(FOutputDevice& Ar) override { Inner->DumpAllocatorStats(Ar); }
		virtual void OnMallocInitialized() override { Inner->OnMallocInitialized(); }
		virtual void OnPreFork() override { Inner->OnPreFork(); }
		virtual void OnPostFork() override { Inner->OnPostFork(); }
#if FLOCK_ENGINE_ALLOCATOR_SPLITS_FREE_CACHED_MEMORY
		virtual uint64 GetImmediatelyFreeableCachedMemorySize() const override { return Inner->GetImmediatelyFreeableCachedMemorySize(); }
		virtual uint64 GetTotalFreeCachedMemorySize() const override { return Inner->GetTotalFreeCachedMemorySize(); }
#elif FLOCK_ENGINE_ALLOCATOR_TELLS_FREE_CACHED_MEMORY
		virtual uint64 GetFreeCachedMemorySize() const override { return Inner->GetFreeCachedMemorySize(); }
#endif
#if FLOCK_ENGINE_ALLOCATOR_MARKS_ITS_CACHES
		virtual void* MallocZeroed(SIZE_T Count, uint32 Alignment) override { CountIfOnTheGameThread(); return Inner->MallocZeroed(Count, Alignment); }
		virtual void* TryMallocZeroed(SIZE_T Count, uint32 Alignment) override { CountIfOnTheGameThread(); return Inner->TryMallocZeroed(Count, Alignment); }
		virtual void MarkTLSCachesAsUsedOnCurrentThread() override { Inner->MarkTLSCachesAsUsedOnCurrentThread(); }
		virtual void MarkTLSCachesAsUnusedOnCurrentThread() override { Inner->MarkTLSCachesAsUnusedOnCurrentThread(); }
#endif

	private:
		void CountIfOnTheGameThread()
		{
			if (IsInGameThread())
			{
				++GameThreadAllocations;
			}
		}

		FMalloc* Inner;
	};

	/** Installs a counter in place of the engine's allocator for its own life. */
	struct FAllocationsCounted
	{
		FMalloc* Before = FLOCK_ENGINE_ALLOCATOR;
		FCountingAllocator Counter{ Before };

		FAllocationsCounted() { FLOCK_ENGINE_ALLOCATOR = &Counter; }
		~FAllocationsCounted() { FLOCK_ENGINE_ALLOCATOR = Before; }

		int32 Taken() { return Counter.GameThreadAllocations.exchange(0); }
	};

	/** Relay connections over a fake relay on loopback; each is closed when the test ends. */
	struct FRelayFixture
	{
		TUniquePtr<FFlockFakeRelayServer> Relay = MakeUnique<FFlockFakeRelayServer>();
		TSharedRef<FFlockRecordingLogger> Log = MakeShared<FFlockRecordingLogger>();
		TArray<TSharedRef<FFlockRelayConnection>> Connections;

		FRelayFixture() { FFlockUdpSockets::ResetForTesting(); }

		~FRelayFixture()
		{
			for (const TSharedRef<FFlockRelayConnection>& Connection : Connections)
			{
				Connection->Close();
			}
			Connections.Reset();
			FFlockRelayConnection::MostOpeningsForTesting = 0;
			FFlockRelayConnection::bThreadRefusedForTesting = false;
			FFlockUdpSockets::ResetForTesting();
		}

		FFlockRelayLogin Login(const FString& Player, const FString& Host = TEXT("127.0.0.1")) const
		{
			FFlockRelayLogin Made;
			Made.Host = Host;
			Made.Port = Relay->GetPort();
			Made.Username = TEXT("1760000000:game:") + Player;
			Made.Password = TEXT("minted-for-the-test");
			return Made;
		}

		TSharedRef<FOpening> Open(const FFlockRelayLogin& WithLogin, const FFlockRelayTiming& Timing = Quick())
		{
			const TSharedRef<FOpening> Answer = MakeShared<FOpening>();
			Connections.Add(FFlockRelayConnection::Open(WithLogin, TArray<FFlockRelayStunLookUp>(), Timing, Log, Into(Answer)));
			return Answer;
		}

		TSharedRef<FOpening> Open(const FString& Player, const FFlockRelayTiming& Timing = Quick()) { return Open(Login(Player), Timing); }

		/** One frame: every connection hands the game what it has. */
		void Frame()
		{
			const TArray<TSharedRef<FFlockRelayConnection>> Each = Connections;
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

using namespace FlockRelayTestHelpers;
using FSeen = FFlockFakeRelayServer::FSeen;

// ---- reserving an address ----

FLOCK_RELAY_TEST(FFlockRelayReservesTest, "OpenReservesAnAddressAfterAnsweringTheLoginChallenge")
bool FFlockRelayReservesTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	if (!TestTrue(TEXT("precondition: a relay on loopback"), F.Relay->IsReady()))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("open"), Opened->IsOpen());
	TestEqualSensitive(TEXT("the relay address the relay answered"), Opened->GetAddress().ToString(), FString::Printf(TEXT("%s:50000"), FFlockFakeRelayServer::RelayIp));
	const TArray<FSeen> Asked = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate);
	if (TestEqual(TEXT("one unsigned reservation, challenged, then one signed with the login"), Asked.Num(), 2))
	{
		TestFalse(TEXT("the first unsigned"), Asked[0].bSigned);
		TestTrue(TEXT("the second signed"), Asked[1].bSigned);
	}
	TestEqual(TEXT("one reservation"), F.Relay->Reservations(), 1);
	TestEqualSensitive(TEXT("the server it is on, as the login names it"), Opened->GetServer(), FString::Printf(TEXT("127.0.0.1:%d"), F.Relay->GetPort()));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayLostRequestTest, "OpenALostRequestIsSentAgainUnderItsTransaction")
bool FFlockRelayLostRequestTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->DropNext(FFlockFakeRelayServer::MethodAllocate, 1);
	if (!F.Opened(*this, RelayPlayerA).IsValid())
	{
		return false;
	}
	const TArray<FSeen> Signed = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; });
	if (TestEqual(TEXT("the signed reservation went twice"), Signed.Num(), 2))
	{
		TestEqualSensitive(TEXT("under the same transaction, so the relay knows it for a repeat"), Signed[1].Transaction, Signed[0].Transaction);
	}
	TestEqual(TEXT("one reservation, not two"), F.Relay->Reservations(), 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelaySilentTest, "OpenSilentRelayFailsUnreachableAfterTheSocketRoundsAndTheAnswerWait")
bool FFlockRelaySilentTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->bSilent = true;
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.3;
	const double Started = FPlatformTime::Seconds();
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA, Timing);
	F.Until([&]() { return Opening->bAnswered; });
	const double Took = FPlatformTime::Seconds() - Started;
	TestEqualSensitive(TEXT("unreachable"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Unreachable));
	TestTrue(FString::Printf(TEXT("three 0.3 s rounds of sockets went unanswered, then the 2 s answer wait from a new socket (took %.2f s)"), Took),
		Took >= 2.7 && Took <= 4.9);
	const TArray<FSeen> Asked = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate);
	TestEqual(TEXT("sent at 0, 0.5 and 1.5 s before giving up at 2 s"), Asked.Num(), 3);
	TSet<FString> Transactions;
	for (const FSeen& Seen : Asked)
	{
		Transactions.Add(Seen.Transaction);
	}
	TestEqual(TEXT("every send under one transaction"), Transactions.Num(), 1);
	TestFalse(TEXT("the failure says how to fix it"), Opening->Result.Failure.Error.Hint.IsEmpty());
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayLookUpHangsTest, "OpenNameLookUpThatNeverAnswersFailsUnreachableWithinItsWait")
bool FFlockRelayLookUpHangsTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	FFlockRelayTiming Timing = Quick();
	Timing.NameLookUpWaitSeconds = 0.5;
	Timing.NameLookUpForTesting = [](const FString&, TFunction<void(const FString&)>) {};
	const double Started = FPlatformTime::Seconds();
	const TSharedRef<FOpening> Opening = F.Open(F.Login(RelayPlayerA, TEXT("relay.test")), Timing);
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("unreachable"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Unreachable));
	TestTrue(TEXT("given up at the lookup's own 0.5 s wait"), FPlatformTime::Seconds() - Started < 2.0);
	TestTrue(TEXT("the message names the server"), Opening->Result.Failure.Error.Message.Contains(TEXT("relay.test")));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayWrongPasswordTest, "OpenWrongPasswordFailsWrongLoginWithoutAnotherTry")
bool FFlockRelayWrongPasswordTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->Password = TEXT("not-what-flock-minted");
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("wrong login"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::WrongLogin));
	F.RealSeconds(0.5);
	TestEqual(TEXT("a refused login is not sent again"), F.Signed(FFlockFakeRelayServer::MethodAllocate), 1);
	TestEqual(TEXT("no reservation"), F.Relay->Reservations(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayFullTest, "OpenRelayWithNoRoomFailsFull")
bool FFlockRelayFullTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->RefuseReservationsWith = 486;
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("full"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Full));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayRefusedTest, "OpenRefusedForAnotherReasonFailsRefused")
bool FFlockRelayRefusedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->RefuseReservationsWith = 442;
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("refused"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Refused));
	TestTrue(TEXT("the relay's own code is in the reason"), Opening->Result.Failure.Error.Message.Contains(TEXT("442")));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayStaleNonceTest, "OpenStaleNonceIsSentAgainWithTheNewNonce")
bool FFlockRelayStaleNonceTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->AnswerStaleNonce(1);
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const TArray<FSeen> Signed = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; });
	if (TestEqual(TEXT("signed once with the first nonce, once with the one the 438 gave"), Signed.Num(), 2))
	{
		TestNotEqual(TEXT("a new request, not a repeat"), Signed[1].Transaction, Signed[0].Transaction);
	}
	TestEqual(TEXT("the login challenge's nonce, then the 438's"), Opened->GetNoncesTakenForTesting(), 2);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayStaleNonceAlwaysTest, "OpenStaleNonceEveryTimeGivesUp")
bool FFlockRelayStaleNonceAlwaysTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->AnswerStaleNonce(-1);
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("refused"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Refused));
	F.RealSeconds(0.5);
	TestEqual(TEXT("the challenge and two nonces, then no more"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num(), 3);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayForgedTest, "OpenAnAnswerTheRelayDidNotSignIsNotBelieved")
bool FFlockRelayForgedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->bForgeNextReservation = true;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestEqualSensitive(TEXT("the address the relay signed, not the one that came first"), Opened->GetAddress().ToString(),
		FString::Printf(TEXT("%s:50000"), FFlockFakeRelayServer::RelayIp));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayCloseWhileOpeningTest, "OpenClosedWhileWaitingGivesTheReservationBack")
bool FFlockRelayCloseWhileOpeningTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->bHoldReservationAnswers = true;
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return F.Signed(FFlockFakeRelayServer::MethodAllocate) > 0; });
	TestEqual(TEXT("precondition: the relay holds an address whose answer is on hold"), F.Relay->Reservations(), 1);

	F.Connections[0]->Close();
	TestTrue(TEXT("the open ends at once"), Opening->bAnswered);
	TestTrue(TEXT("as cancelled"), Opening->Result.Failure.IsCancelled());
	F.Until([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	TestEqual(TEXT("one release"), F.Relay->Releases(), 1);
	F.Relay->ReleaseHeldAnswers();
	F.RealSeconds(0.2);
	TestEqual(TEXT("answered once"), Opening->Times, 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayGameThreadTest, "TheOpeningIsAnsweredOnTheGameThreadAndOnlyFromATick")
bool FFlockRelayGameThreadTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->bHoldReservationAnswers = true;
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return F.Signed(FFlockFakeRelayServer::MethodAllocate) > 0; });
	F.Relay->ReleaseHeldAnswers();
	// The relay's thread takes the answer at once; without a frame nothing reaches the game.
	const double Ends = FPlatformTime::Seconds() + 0.5;
	while (!F.Connections[0]->IsOpen() && FPlatformTime::Seconds() < Ends)
	{
		FPlatformProcess::Sleep(0.005f);
	}
	FPlatformProcess::Sleep(0.1f);
	TestTrue(TEXT("precondition: the relay's thread opened it"), F.Connections[0]->IsOpen());
	TestFalse(TEXT("game code never runs on the relay's thread: nothing answered until the game's frame"), Opening->bAnswered);
	F.Frame();
	TestTrue(TEXT("answered in the frame"), Opening->bAnswered && Opening->Result.bSuccess);
	TestTrue(TEXT("on the game thread"), Opening->AnsweredOnThread == GGameThreadId);
	const uint32 RelayThread = F.Connections[0]->GetThreadIdForTesting();
	TestTrue(TEXT("the relay has a thread of its own"), RelayThread != 0 && RelayThread != GGameThreadId);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayDefaultTimingTest, "TheTimingAGameGetsIsTheMeasuredOne")
bool FFlockRelayDefaultTimingTest::RunTest(const FString& Parameters)
{
	// Every other test shortens these; a game gets these.
	const FFlockRelayTiming Timing;
	TestEqual(TEXT("a name lookup waits 5 s"), Timing.NameLookUpWaitSeconds, 5.0);
	TestEqual(TEXT("an answer waits 5 s"), Timing.AnswerWaitSeconds, 5.0);
	TestEqual(TEXT("8 sockets at once"), Timing.SocketsTriedAtOnce, 8);
	TestEqual(TEXT("3 rounds"), Timing.RoundsOfSocketsTried, 3);
	TestEqual(TEXT("of a second"), Timing.SocketRoundWaitSeconds, 1.0);
	TestEqual(TEXT("the reservation renewed at least every 4 min"), Timing.LongestBetweenReservationRenewalsSeconds, 240.0);
	TestEqual(TEXT("openings every 4 min"), Timing.RenewOpeningsEverySeconds, 240.0);
	TestEqual(TEXT("channels every 8 min"), Timing.RenewChannelsEverySeconds, 480.0);
	TestEqual(TEXT("an opening lasts 5 min"), Timing.OpeningLastsSeconds, 300.0);
	TestEqual(TEXT("a channel 10 min"), Timing.ChannelLastsSeconds, 600.0);
	TestEqual(TEXT("a keep-alive after 15 s of quiet"), Timing.KeepAliveAfterSeconds, 15.0);
	TestEqual(TEXT("a release's answer waited 1 s"), Timing.ReleaseAnswerWaitSeconds, 1.0);
	TestEqual(TEXT("the most IPs an address opens to: 64 players and the host"), FFlockRelayConnection::MostOpenings, 65);
	TestEqual(TEXT("the most bytes a packet carries"), FFlockRelayConnection::MostBytesInAPacket, 1400);
	return true;
}

// ---- packets ----

FLOCK_RELAY_TEST(FFlockRelayTradeTest, "HostAndPlayerTradePacketsOnChannelsOnceAsked")
bool FFlockRelayTradeTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FFlockRelayPeer HeardPlayer;
	FFlockRelayPeer HeardHost;
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("hello host"), &HeardPlayer);
	TestTrue(TEXT("the host hears the player from the player's relay address"), HeardPlayer == Player->GetAddress());
	F.Delivered(*this, Host, HeardPlayer, Player, TEXT("hello player"), &HeardHost);
	TestTrue(TEXT("the player hears the host from the host's relay address"), HeardHost == Host->GetAddress());
	F.RealSeconds(0.3);
	TestEqual(TEXT("sending and hearing bind no channel"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num(), 0);

	Player->KeepChannelTo(Host->GetAddress());
	Host->KeepChannelTo(Player->GetAddress());
	F.Until([&]() { return F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num() >= 2; }, 3.0);
	F.RealSeconds(0.3);
	const int32 FramesBefore = F.Relay->Count(FFlockFakeRelayServer::ChannelFrame, 0);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		F.Delivered(*this, Player, Host->GetAddress(), Host, FString::Printf(TEXT("to host %d"), Index));
		F.Delivered(*this, Host, Player->GetAddress(), Player, FString::Printf(TEXT("to player %d"), Index));
	}
	TestEqual(TEXT("once bound, every packet goes in a channel frame"), F.Relay->Count(FFlockFakeRelayServer::ChannelFrame, 0) - FramesBefore, 20);
	TestEqual(TEXT("none dropped"), Host->GetPacketsDropped() + Player->GetPacketsDropped(), 0ll);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelaySendNeverOpenedTest, "SendToAnAddressNeverOpenedIsRefused")
bool FFlockRelaySendNeverOpenedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const uint8 Payload[] = { 1, 2, 3 };
	const FFlockRelayPeer Other = Peer(FString::Printf(TEXT("%s:50001"), FFlockFakeRelayServer::RelayIp));
	TestFalse(TEXT("nothing goes to an IP never opened"), Opened->Send(Other, Payload, 3));
	const TSharedRef<FOpenedTo> Opening = F.OpenTo(Opened, Opened->GetAddress());
	F.Until([&]() { return Opening->bAnswered; });
	TestTrue(TEXT("control: once opened, it goes"), Opened->Send(Other, Payload, 3));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelaySendSizesTest, "SendEmptyOrOverTheMostIsRefused")
bool FFlockRelaySendSizesTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const TSharedRef<FOpenedTo> Opening = F.OpenTo(Opened, Opened->GetAddress());
	F.Until([&]() { return Opening->bAnswered; });
	TArray<uint8> Big;
	Big.SetNumZeroed(FFlockRelayConnection::MostBytesInAPacket + 1);
	TestFalse(TEXT("an empty packet"), Opened->Send(Opened->GetAddress(), Big.GetData(), 0));
	TestFalse(TEXT("one byte over the most"), Opened->Send(Opened->GetAddress(), Big.GetData(), Big.Num()));
	TestTrue(TEXT("control: the most"), Opened->Send(Opened->GetAddress(), Big.GetData(), FFlockRelayConnection::MostBytesInAPacket));
	TestFalse(TEXT("no data at all"), Opened->Send(Opened->GetAddress(), nullptr, 10));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayFullStoreTest, "ArrivedWhenFullDropsTheNewest")
bool FFlockRelayFullStoreTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	uint8 Packet[8] = {};
	bool bAllSent = true;
	for (int32 Index = 0; Index < 300; ++Index)
	{
		Packet[0] = static_cast<uint8>(Index >> 8);
		Packet[1] = static_cast<uint8>(Index);
		bAllSent &= Player->Send(Host->GetAddress(), Packet, 8);
		if (Index % 20 == 19)
		{
			F.RealSeconds(0.02);
		}
	}
	TestTrue(TEXT("all 300 sent"), bAllSent);
	F.Until([&]() { return Host->GetPacketsDropped() == 44; }, 5.0);
	TestEqual(TEXT("44 packets past the 256 held were dropped"), Host->GetPacketsDropped(), 44ll);
	uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
	TArray<int32> Numbers;
	int32 Count = 0;
	FFlockRelayPeer From;
	while (Host->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From))
	{
		Numbers.Add((Buffer[0] << 8) | Buffer[1]);
	}
	bool bOldestInOrder = Numbers.Num() == 256;
	for (int32 Index = 0; bOldestInOrder && Index < 256; ++Index)
	{
		bOldestInOrder = Numbers[Index] == Index;
	}
	TestTrue(TEXT("the oldest 256, in order"), bOldestInOrder);
	TestFalse(TEXT("a receive buffer smaller than the most is refused"), Host->TryReceive(Buffer, 100, Count, From));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayNoAllocationTest, "PacketsAllocateNothingOnTheGamesThread")
bool FFlockRelayNoAllocationTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("warm up"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("warm up back"));
	Player->KeepChannelTo(Host->GetAddress());
	Host->KeepChannelTo(Player->GetAddress());
	F.Until([&]() { return F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num() >= 2; }, 3.0);
	F.RealSeconds(0.3);

	const FFlockRelayPeer HostAddress = Host->GetAddress();
	const FFlockRelayPeer PlayerAddress = Player->GetAddress();
	uint8 Payload[200] = {};
	uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
	int32 Received = 0;
	int32 Working = 0;
	int32 Control = 0;
	{
		FAllocationsCounted Counted;
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			for (int32 Index = 0; Index < 50; ++Index)
			{
				Player->Send(HostAddress, Payload, UE_ARRAY_COUNT(Payload));
				Host->Send(PlayerAddress, Payload, UE_ARRAY_COUNT(Payload));
			}
			int32 Count = 0;
			FFlockRelayPeer From;
			while (Host->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From))
			{
				++Received;
			}
			while (Player->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From))
			{
				++Received;
			}
			FPlatformProcess::Sleep(0.005f);
		}
		Working = Counted.Taken();
		TArray<TArray<uint8>> Made;
		for (int32 Index = 0; Index < 10; ++Index)
		{
			Made.Add(TArray<uint8>());
			Made.Last().SetNumZeroed(100);
		}
		Control = Counted.Taken();
	}
	TestTrue(FString::Printf(TEXT("control: the count sees allocations (%d)"), Control), Control >= 10);
	TestEqual(TEXT("sending and taking 100 packets a frame for 10 frames allocates nothing on the game thread"), Working, 0);
	TestTrue(FString::Printf(TEXT("control: packets were taken while counted (%d)"), Received), Received > 100);
	return true;
}

// ---- renewals ----

FLOCK_RELAY_TEST(FFlockRelayReservationRenewedTest, "ReservationIsRenewedBeforeItLapses")
bool FFlockRelayReservationRenewedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->ReservationLifetimeSeconds = 2.0;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	F.RealSeconds(4.5);
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("after two lifetimes"));
	TestTrue(TEXT("not lost"), Host->GetFailureReason().IsEmpty());
	const int32 Renewals = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodRefresh).FilterByPredicate([](const FSeen& Seen)
	{
		return Seen.LifetimeAsked.IsSet() && Seen.LifetimeAsked.GetValue() == 600;
	}).Num();
	TestTrue(FString::Printf(TEXT("each side renewed at half the 2 s it was granted (%d renewals)"), Renewals), Renewals >= 6);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayOpeningRenewedTest, "OpeningIsRenewedBeforeItLapses")
bool FFlockRelayOpeningRenewedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->OpeningLifetimeSeconds = 2.0;
	FFlockRelayTiming Timing = Quick();
	Timing.RenewOpeningsEverySeconds = 1.0;
	Timing.OpeningLastsSeconds = 2.0;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player, Timing))
	{
		return false;
	}
	F.RealSeconds(4.5);
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("after two openings' lifetimes"));
	const int32 Openings = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodCreatePermission).Num();
	TestTrue(FString::Printf(TEXT("each side's opening renewed every second (%d)"), Openings), Openings >= 8);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayChannelRenewedTest, "ChannelIsRenewedBeforeItLapses")
bool FFlockRelayChannelRenewedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->ChannelLifetimeSeconds = 2.0;
	FFlockRelayTiming Timing = Quick();
	Timing.RenewChannelsEverySeconds = 1.0;
	Timing.ChannelLastsSeconds = 2.0;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player, Timing))
	{
		return false;
	}
	Player->KeepChannelTo(Host->GetAddress());
	Host->KeepChannelTo(Player->GetAddress());
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("bind"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("bind back"));
	F.RealSeconds(4.5);
	const int32 FramesBefore = F.Relay->Count(FFlockFakeRelayServer::ChannelFrame, 0);
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("after two channel lifetimes"));
	F.Delivered(*this, Host, Player->GetAddress(), Player, TEXT("and back"));
	TestEqual(TEXT("both still go in channel frames"), F.Relay->Count(FFlockFakeRelayServer::ChannelFrame, 0) - FramesBefore, 2);
	const int32 Binds = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num();
	TestTrue(FString::Printf(TEXT("each channel renewed every second (%d)"), Binds), Binds >= 8);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayRenewalStaleNonceTest, "RenewalStaleNonceIsSentAgainAndTheRelayKept")
bool FFlockRelayRenewalStaleNonceTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->ReservationLifetimeSeconds = 2.0;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const int32 RenewalsBefore = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodRefresh).Num();
	F.Relay->AnswerStaleNonce(1);
	F.RealSeconds(3.0);
	TestTrue(TEXT("the relay is kept"), Opened->GetFailureReason().IsEmpty() && Opened->IsOpen());
	TestTrue(TEXT("the renewal went again with the new nonce, and renewals went on"),
		F.Relay->SeenRequests(FFlockFakeRelayServer::MethodRefresh).Num() - RenewalsBefore >= 3);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayRenewalRefusedTest, "RenewalRefusedLosesTheRelayAtOnceAndSaysSoOnce")
bool FFlockRelayRenewalRefusedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// Renewed every half second of a 600 s reservation: only the refusal itself can lose it within the wait.
	FFlockRelayTiming Timing = Quick();
	Timing.LongestBetweenReservationRenewalsSeconds = 0.5;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	if (!Opened.IsValid())
	{
		return false;
	}
	F.Relay->ForgetEveryReservation();
	F.Until([&]() { return Opened->HasStopped(); }, 3.0);
	TestEqualSensitive(TEXT("lost"), Opened->GetFailureReason(), FString(FlockRelayFailureReasons::Lost));
	F.RealSeconds(0.5);
	const TArray<FString> Lost = F.Log->Warnings.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("was lost")); });
	if (TestEqual(TEXT("said once"), Lost.Num(), 1))
	{
		TestTrue(TEXT("with its fix"), Lost[0].Contains(TEXT("Fix:")) || Lost[0].Contains(FlockRelayFailureReasons::FixFor(FlockRelayFailureReasons::Lost)));
	}
	const uint8 One = 1;
	TestFalse(TEXT("nothing more is sent"), Opened->Send(Opened->GetAddress(), &One, 1));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayRenewalUnansweredTest, "RenewalUnansweredLosesTheRelayOnlyOnceItLapses")
bool FFlockRelayRenewalUnansweredTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->ReservationLifetimeSeconds = 3.0;
	FFlockRelayTiming Timing = Quick();
	Timing.AnswerWaitSeconds = 0.5;
	Timing.RetryRenewalAfterSeconds = 0.3;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	if (!Opened.IsValid())
	{
		return false;
	}
	const double Started = FPlatformTime::Seconds();
	F.Relay->bSilent = true;
	F.RealSeconds(2.3);
	TestTrue(TEXT("renewals unanswered for a while lose nothing while the address lasts"), Opened->GetFailureReason().IsEmpty());
	F.Until([&]() { return Opened->HasStopped(); }, 3.0);
	const double Took = FPlatformTime::Seconds() - Started;
	TestEqualSensitive(TEXT("lost"), Opened->GetFailureReason(), FString(FlockRelayFailureReasons::Lost));
	TestTrue(FString::Printf(TEXT("lost when the 3 s it was granted ran out (%.2f s)"), Took), Took >= 2.5 && Took <= 4.5);
	TestTrue(TEXT("a renewal unanswered was tried again"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodRefresh).Num() >= 2);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayKeepAliveTest, "KeepAliveGoesOutWhenNothingElseDoes")
bool FFlockRelayKeepAliveTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	FFlockRelayTiming Timing = Quick();
	Timing.KeepAliveAfterSeconds = 0.5;
	if (!F.Opened(*this, RelayPlayerA, Timing).IsValid())
	{
		return false;
	}
	const int32 Before = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodBinding).Num();
	F.RealSeconds(2.2);
	TestTrue(TEXT("a binding request each half second of quiet"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodBinding).Num() - Before >= 3);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayNoChannelUnaskedTest, "HostBindsNoChannelToAPeerItHearsOrAnswersUntilAsked")
bool FFlockRelayNoChannelUnaskedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	FFlockRelayPeer Heard;
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("from someone on the relay"), &Heard);
	F.Delivered(*this, Host, Heard, Player, TEXT("an answer"));
	F.RealSeconds(0.5);
	TestEqual(TEXT("strangers on the relay get no channel: only a peer known to be a player"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num(), 0);

	Host->KeepChannelTo(Heard);
	F.Until([&]() { return F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind).Num() >= 1; }, 3.0);
	const TArray<FSeen> Binds = F.Relay->SeenRequests(FFlockFakeRelayServer::MethodChannelBind);
	TestTrue(TEXT("the host bound a channel once asked, from its own socket"), Binds.Num() >= 1 && Binds[0].ClientPort == Host->GetLocalPortForTesting());
	TestTrue(TEXT("kept"), Host->KeepsChannelToForTesting(Heard));
	Host->LetChannelGo(Heard);
	TestFalse(TEXT("let go: no longer renewed"), Host->KeepsChannelToForTesting(Heard));
	return true;
}

namespace FlockRelayTestHelpers
{
	/** Keeps what it is handed, under a lock: it runs on the relay's thread. */
	class FHandedReceiver : public IFlockRelayReceiver
	{
	public:
		virtual void Arrived(const FFlockRelayPeer& From, const uint8* Data, int32 Count) override
		{
			FScopeLock Locked(&Lock);
			Handed.Add(From.ToString() + TEXT(" ") + FFlockRelayWire::FromUtf8(Data, Count));
		}

		TArray<FString> Copy() const
		{
			FScopeLock Locked(&Lock);
			return Handed;
		}

	private:
		mutable FCriticalSection Lock;
		TArray<FString> Handed;
	};
}

FLOCK_RELAY_TEST(FFlockRelayHandOverTest, "ArrivalsHandedOverReachTheReceiverAndAreKeptAgainOnceItStops")
bool FFlockRelayHandOverTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	const TSharedRef<FHandedReceiver, ESPMode::ThreadSafe> Receiver = MakeShared<FHandedReceiver, ESPMode::ThreadSafe>();
	const TSharedRef<FHandedReceiver, ESPMode::ThreadSafe> Another = MakeShared<FHandedReceiver, ESPMode::ThreadSafe>();
	TestTrue(TEXT("handed to the receiver"), Host->HandArrivalsTo(Receiver));
	TestFalse(TEXT("one receiver at a time"), Host->HandArrivalsTo(Another));
	const TArray<uint8> Handed = Bytes(TEXT("handed"));
	TestTrue(TEXT("sent"), Player->Send(Host->GetAddress(), Handed.GetData(), Handed.Num()));
	F.Until([&]() { return Receiver->Copy().Num() == 1; }, 3.0);
	const TArray<FString> Heard = Receiver->Copy();
	TestTrue(TEXT("the receiver was handed the packet, with who sent it"), Heard.Num() == 1 && Heard[0] == Player->GetAddress().ToString() + TEXT(" handed"));
	uint8 Buffer[FFlockRelayConnection::MostBytesInAPacket];
	int32 Count = 0;
	FFlockRelayPeer From;
	TestFalse(TEXT("nothing kept while handed over"), Host->TryReceive(Buffer, UE_ARRAY_COUNT(Buffer), Count, From));

	Host->StopHandingArrivalsTo(Another);
	TestTrue(TEXT("only the receiver itself stops it"), Host->HandsArrivalsOver());
	TestFalse(TEXT("no packet store made while every arrival is handed over"), Host->KeepsPacketsForTesting());
	Host->StopHandingArrivalsTo(Receiver);
	F.Delivered(*this, Player, Host->GetAddress(), Host, TEXT("kept again"));
	TestEqual(TEXT("the receiver heard no more"), Receiver->Copy().Num(), 1);
	TestTrue(TEXT("made when the first packet is kept"), Host->KeepsPacketsForTesting());
	return true;
}

// ---- opening to other relay addresses ----

FLOCK_RELAY_TEST(FFlockRelayOpenRefusedTest, "OpenToRefusedFailsOpenRefusedAndKeepsTheAddress")
bool FFlockRelayOpenRefusedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->RefusedOpenings.Add(FFlockFakeRelayServer::RelayIp);
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const TSharedRef<FOpenedTo> Opening = F.OpenTo(Opened, Opened->GetAddress());
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("open refused"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::OpenRefused));
	TestTrue(TEXT("the relay address stays"), Opened->IsOpen());
	TestTrue(TEXT("remembered"), Opened->RefusedToOpenTo(Opened->GetAddress()));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayOpenUnansweredTest, "OpenToUnansweredFailsUnreachable")
bool FFlockRelayOpenUnansweredTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->DropNext(FFlockFakeRelayServer::MethodCreatePermission, 10);
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const TSharedRef<FOpenedTo> Opening = F.OpenTo(Opened, Opened->GetAddress());
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("unreachable"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Unreachable));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayOpenNoCodeTest, "OpenToARefusalNamingNoCodeIsARefusal")
bool FFlockRelayOpenNoCodeTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->bRefuseOpeningsNamingNoCode = true;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const TSharedRef<FOpenedTo> Opening = F.OpenTo(Opened, Opened->GetAddress());
	F.Until([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("never taken for the success 0 stands for"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Refused));
	const uint8 One = 1;
	TestFalse(TEXT("nothing was opened"), Opened->Send(Opened->GetAddress(), &One, 1));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayOpenCancelTest, "OpenToACallerThatGivesUpIsAnsweredAloneAndTheOpeningGoesOn")
bool FFlockRelayOpenCancelTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	F.Relay->AnswerDelaySeconds = 0.3;
	FFlockRequestHandle GivingUp;
	const TSharedRef<FOpenedTo> First = F.OpenTo(Opened, Opened->GetAddress(), &GivingUp);
	const TSharedRef<FOpenedTo> Second = F.OpenTo(Opened, Opened->GetAddress());
	GivingUp.Cancel();
	TestTrue(TEXT("the caller that gave up is answered at once"), First->bAnswered && First->Result.Failure.IsCancelled());
	TestFalse(TEXT("the other still waits"), Second->bAnswered);
	F.Until([&]() { return Second->bAnswered; });
	TestTrue(TEXT("the opening went on for the other"), Second->Result.bSuccess);
	TestTrue(TEXT("and what it opened is kept"), Opened->IsOpenTo(Opened->GetAddress()));
	return true;
}

// ---- closing ----

FLOCK_RELAY_TEST(FFlockRelayCloseTest, "CloseGivesTheReservationBackWithoutAWarning")
bool FFlockRelayCloseTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("precondition: reserved"), F.Relay->Reservations(), 1);
	Opened->Close();
	F.Until([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	TestEqual(TEXT("one release"), F.Relay->Releases(), 1);
	TestTrue(TEXT("stopped"), Opened->HasStopped());
	TestTrue(TEXT("a close is no failure"), Opened->GetFailureReason().IsEmpty());
	const uint8 One = 1;
	TestFalse(TEXT("nothing more is sent"), Opened->Send(Opened->GetAddress(), &One, 1));
	TestEqual(TEXT("a close is not reported as lost"), F.Log->Warnings.Num(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayStoppedSendTest, "SendAfterTheRelayStoppedIsRefusedWithoutAllocating")
bool FFlockRelayStoppedSendTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	TSharedPtr<FFlockRelayConnection> Host;
	TSharedPtr<FFlockRelayConnection> Player;
	if (!F.HostAndPlayer(*this, Host, Player))
	{
		return false;
	}
	Player->Close();
	const FFlockRelayPeer HostAddress = Host->GetAddress();
	uint8 Payload[200] = {};
	bool bAnySent = false;
	int32 Allocations = 0;
	{
		FAllocationsCounted Counted;
		for (int32 Index = 0; Index < 500; ++Index)
		{
			bAnySent |= Player->Send(HostAddress, Payload, UE_ARRAY_COUNT(Payload));
		}
		Allocations = Counted.Taken();
	}
	TestFalse(TEXT("nothing goes once the relay stopped"), bAnySent);
	TestEqual(TEXT("a game still sending after the stop allocates nothing"), Allocations, 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayReleaseStaleNonceTest, "CloseWithAStaleNonceSendsTheReleaseAgain")
bool FFlockRelayReleaseStaleNonceTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	F.Relay->AnswerStaleNonce(1);
	// Later than one of the relay thread's waits, so only waiting for the release's answer hears the 438.
	F.Relay->AnswerDelaySeconds = 0.15;
	Opened->Close();
	F.Until([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	TestEqual(TEXT("refused as stale, then sent again with the new nonce"), F.Relay->Releases(), 2);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayReleaseAtOnceTest, "CloseWithAStaleNonceAnsweredBeforeTheClosingThreadGoesOnSendsTheReleaseAgain")
bool FFlockRelayReleaseAtOnceTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	// The closing thread is held just after the release goes, as a busy game thread can be: the relay's 438 arrives meanwhile, and
	// the release must already be known as such.
	F.Relay->AnswerStaleNonce(1);
	FFlockRelayRequests::AfterTheReleaseGoesForTesting = []() { FPlatformProcess::Sleep(0.2f); };
	Opened->Close();
	FFlockRelayRequests::AfterTheReleaseGoesForTesting = nullptr;
	F.Until([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	TestEqual(TEXT("refused as stale, then sent again with the new nonce"), F.Relay->Releases(), 2);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayReservationOnItsWayTest, "CloseWhileTheSignedReservationIsAboutToGoKeepsNoAddress")
bool FFlockRelayReservationOnItsWayTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// The relay's thread is held just before the signed reservation goes, and the game closes the relay then: no reservation has
	// gone, so the close sends no release, and the reservation must not go after it.
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bHeld = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bClosed = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	FFlockRelayRequests::BeforeASignedReservationGoesForTesting = [bHeld, bClosed]()
	{
		*bHeld = true;
		const double Until = FPlatformTime::Seconds() + 5.0;
		while (!bClosed->load() && FPlatformTime::Seconds() < Until)
		{
			FPlatformProcess::Sleep(0.001f);
		}
	};
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return bHeld->load(); }, 5.0);
	TestTrue(TEXT("precondition: the relay's thread is about to send the signed reservation"), bHeld->load());
	F.Connections[0]->Close();
	*bClosed = true;
	TestTrue(TEXT("the open ends as cancelled"), Opening->bAnswered && Opening->Result.Failure.IsCancelled());
	F.RealSeconds(0.6);
	FFlockRelayRequests::BeforeASignedReservationGoesForTesting = nullptr;
	TestEqual(TEXT("no address kept"), F.Relay->Reservations(), 0);
	TestEqual(TEXT("the signed reservation never went: the stop came first"), F.Signed(FFlockFakeRelayServer::MethodAllocate), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayJustWentTest, "CloseJustAfterTheSignedReservationWentGivesItBack")
bool FFlockRelayJustWentTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// The relay's thread is held just after the signed reservation went, before its answer: the game closes the relay then,
	// and the close must know a reservation went, to give it back.
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bHeld = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bClosed = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	FFlockRelayRequests::AfterASignedReservationWentForTesting = [bHeld, bClosed]()
	{
		*bHeld = true;
		const double Until = FPlatformTime::Seconds() + 5.0;
		while (!bClosed->load() && FPlatformTime::Seconds() < Until)
		{
			FPlatformProcess::Sleep(0.001f);
		}
	};
	F.Open(RelayPlayerA);
	F.Until([&]() { return bHeld->load(); }, 5.0);
	TestTrue(TEXT("precondition: the signed reservation just went"), bHeld->load());
	F.Connections[0]->Close();
	*bClosed = true;
	F.RealSeconds(0.6);
	FFlockRelayRequests::AfterASignedReservationWentForTesting = nullptr;
	TestEqual(TEXT("precondition: the relay made the reservation"), F.Signed(FFlockFakeRelayServer::MethodAllocate), 1);
	TestEqual(TEXT("given back"), F.Relay->Releases(), 1);
	TestEqual(TEXT("no address kept"), F.Relay->Reservations(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayGoingOutTest, "CloseWhileTheSignedReservationIsGoingOutGivesItBackAfterIt")
bool FFlockRelayGoingOutTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// The relay's thread is held between the sending check and the send of the signed reservation, and the game closes the relay
	// then: the close waits for the reservation to go, so the release reaches the relay after it and gives it back.
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bHeld = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	FFlockRelayRequests::WhileASignedReservationGoesForTesting = [bHeld]()
	{
		*bHeld = true;
		FPlatformProcess::Sleep(0.2f);
	};
	F.Open(RelayPlayerA);
	F.Until([&]() { return bHeld->load(); }, 5.0);
	TestTrue(TEXT("precondition: the signed reservation is going out"), bHeld->load());
	F.Connections[0]->Close();
	FFlockRelayRequests::WhileASignedReservationGoesForTesting = nullptr;
	F.RealSeconds(0.6);
	const TArray<FSeen> Seen = F.Relay->AllSeen();
	const int32 Reservation = Seen.IndexOfByPredicate([](const FSeen& Each) { return Each.Method == FFlockFakeRelayServer::MethodAllocate && Each.bSigned; });
	const int32 Release = Seen.IndexOfByPredicate([](const FSeen& Each)
	{
		return Each.Method == FFlockFakeRelayServer::MethodRefresh && Each.LifetimeAsked.IsSet() && Each.LifetimeAsked.GetValue() == 0;
	});
	TestTrue(TEXT("precondition: the signed reservation went"), Reservation != INDEX_NONE);
	TestTrue(TEXT("The release reached the relay after the reservation"), Release != INDEX_NONE && Release > Reservation);
	TestEqual(TEXT("no address kept"), F.Relay->Reservations(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayNothingAfterLossTest, "NothingButTheReleaseGoesAfterTheRelayIsLost")
bool FFlockRelayNothingAfterLossTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// A keep-alive due at every turn, and the reservation renewed every half second: the turn that hears the refusal would send
	// one after the release if the stop did not end the sending.
	FFlockRelayTiming Timing = Quick();
	Timing.KeepAliveAfterSeconds = 0.0;
	Timing.LongestBetweenReservationRenewalsSeconds = 0.5;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	if (!Opened.IsValid())
	{
		return false;
	}
	F.Relay->ForgetEveryReservation();
	F.Until([&]() { return Opened->HasStopped(); }, 3.0);
	F.RealSeconds(0.3);
	TestEqualSensitive(TEXT("lost"), Opened->GetFailureReason(), FString(FlockRelayFailureReasons::Lost));
	const TArray<FSeen> Seen = F.Relay->AllSeen();
	const int32 FirstRelease = Seen.IndexOfByPredicate([](const FSeen& Each)
	{
		return Each.Method == FFlockFakeRelayServer::MethodRefresh && Each.LifetimeAsked.IsSet() && Each.LifetimeAsked.GetValue() == 0;
	});
	if (!TestTrue(TEXT("precondition: the release went"), FirstRelease != INDEX_NONE))
	{
		return false;
	}
	int32 AfterIt = 0;
	for (int32 Index = FirstRelease + 1; Index < Seen.Num(); ++Index)
	{
		const bool bRelease = Seen[Index].Method == FFlockFakeRelayServer::MethodRefresh && Seen[Index].LifetimeAsked.IsSet()
			&& Seen[Index].LifetimeAsked.GetValue() == 0;
		AfterIt += bRelease ? 0 : 1;
	}
	TestEqual(TEXT("nothing but the release went after it"), AfterIt, 0);
	TestTrue(TEXT("control: keep-alives went while the relay worked"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodBinding).Num() > 10);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayNoThreadTest, "WithoutAThreadTheOpeningFailsAndSaysWhy")
bool FFlockRelayNoThreadTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	FFlockRelayConnection::bThreadRefusedForTesting = true;
	const int32 ThreadsBefore = FFlockRelayConnection::GetRelayThreadsForTesting();
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA);
	F.Until([&]() { return Opening->bAnswered; }, 2.0);
	FFlockRelayConnection::bThreadRefusedForTesting = false;
	TestTrue(TEXT("answered"), Opening->bAnswered && !Opening->Result.bSuccess);
	TestEqualSensitive(TEXT("as a relay with no thread"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::NoThread));
	TestFalse(TEXT("with its fix"), Opening->Result.Failure.Error.Hint.IsEmpty());
	TestEqual(TEXT("no relay thread kept for it"), FFlockRelayConnection::GetRelayThreadsForTesting(), ThreadsBefore);
	TestEqual(TEXT("nothing was asked of the relay"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayAfterCloseTest, "OpenToAfterCloseSaysClosed")
bool FFlockRelayAfterCloseTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	Opened->Close();
	const TSharedRef<FOpenedTo> Opening = F.OpenTo(Opened, Opened->GetAddress());
	TestTrue(TEXT("answered at once"), Opening->bAnswered);
	TestEqualSensitive(TEXT("closed, not lost"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Closed));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayCloseWaitingTest, "CloseEndsACallStillWaitingAsCancelled")
bool FFlockRelayCloseWaitingTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->DropNext(FFlockFakeRelayServer::MethodCreatePermission, 10);
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const TSharedRef<FOpenedTo> Opening = F.OpenTo(Opened, Opened->GetAddress());
	F.RealSeconds(0.2);
	TestFalse(TEXT("precondition: still waiting"), Opening->bAnswered);
	Opened->Close();
	TestTrue(TEXT("cancelled, at once"), Opening->bAnswered && Opening->Result.Failure.IsCancelled());
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayDroppedTest, "AConnectionItsOwnerDropsGivesItsAddressBackAndItsThreadEnds")
bool FFlockRelayDroppedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	if (!F.Opened(*this, RelayPlayerA).IsValid())
	{
		return false;
	}
	TestEqual(TEXT("precondition: reserved"), F.Relay->Reservations(), 1);
	F.Connections.Reset();
	F.Until([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	F.Until([]() { return FFlockRelayConnection::GetRelayThreadsForTesting() == 0; }, 3.0);
	TestEqual(TEXT("no relay thread left"), FFlockRelayConnection::GetRelayThreadsForTesting(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayModuleStopsTest, "FlocksModuleGoingAwayStopsEveryRelayThreadAndGivesTheAddressesBack")
bool FFlockRelayModuleStopsTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("precondition: a relay thread runs"), FFlockRelayConnection::GetRelayThreadsForTesting() >= 1);
	// A relay that no longer answers: waiting for the release's answer would take its whole second.
	F.Relay->bSilent = true;
	const double Started = FPlatformTime::Seconds();
	FFlockRelayConnection::StopEveryRelayThread();
	const double Took = FPlatformTime::Seconds() - Started;
	TestTrue(FString::Printf(TEXT("waited for at once, not for the release's answer (%.2f s)"), Took), Took < 0.5);
	TestEqual(TEXT("no relay thread left"), FFlockRelayConnection::GetRelayThreadsForTesting(), 0);
	TestTrue(TEXT("the relay its owner never closed stopped"), Opened->HasStopped());
	F.Until([&]() { return F.Relay->Releases() == 1; }, 1.0);
	TestEqual(TEXT("and sent its release before the thread was let go"), F.Relay->Releases(), 1);
	return true;
}

// ---- reaching a relay that answers only some ports ----

FLOCK_RELAY_TEST(FFlockRelaySixthPortTest, "Reach.ARelayThatAnswersOnlyTheSixthPortItHearsIsReservedFromThatPort")
bool FFlockRelaySixthPortTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->AnswersOnlyPortsHeard = TSet<int32>({ 6 });
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.4;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	TestTrue(TEXT("open"), Opened.IsValid() && Opened->IsOpen());
	TestEqual(TEXT("reserved from the one port the relay answers"), F.Relay->Reservations(), 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayLaterRoundTest, "Reach.ARelayThatAnswersNoPortOfTheFirstRoundIsReservedFromALaterOne")
bool FFlockRelayLaterRoundTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// The first round tries 8 ports; the 11th port heard is the third of the second round.
	F.Relay->AnswersOnlyPortsHeard = TSet<int32>({ 11 });
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.4;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	TestTrue(TEXT("open"), Opened.IsValid() && Opened->IsOpen());
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayNoBindingTest, "Reach.ARelayThatAnswersNoBindingIsStillReservedFromANewSocketAfterEveryRound")
bool FFlockRelayNoBindingTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->bAnswersBindings = false;
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.2;
	const double Started = FPlatformTime::Seconds();
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("a relay that ignores bindings is reserved all the same"), Opened->IsOpen());
	TestTrue(TEXT("the socket made when no search socket was answered is dual-stack too"),
		Opened->GetSocketProtocolForTesting() == ProtocolThisMachineGivesARelaySocket());
	TestTrue(TEXT("after every round went unanswered"), FPlatformTime::Seconds() - Started >= 0.55);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayNoPortTest, "Reach.ARelayThatAnswersNoPortAtAllFailsAsUnreachable")
bool FFlockRelayNoPortTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->AnswersOnlyPortsHeard = TSet<int32>();
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.4;
	const TSharedRef<FOpening> Opening = F.Open(RelayPlayerA, Timing);
	F.Until([&]() { return Opening->bAnswered; }, 15.0);
	TestEqualSensitive(TEXT("unreachable"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Unreachable));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayDualStackTest, "Reach.ARelayIsReachedFromADualStackSocketWhereThePlatformAllows")
bool FFlockRelayDualStackTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	// A phone on mobile data heard Cloudflare's STUN server only on dual-stack sockets (measured): the search's sockets are dual-stack too.
	F.Relay->AnswersOnlyPortsHeard = TSet<int32>({ 3 });
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.4;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("dual-stack"), Opened->GetSocketProtocolForTesting() == ProtocolThisMachineGivesARelaySocket());
	TestEqual(TEXT("reserved from the one port the relay answers, through the IPv4 address in its IPv6 form"), F.Relay->Reservations(), 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayConnectRefusedTest, "Reach.ARelayADualStackSocketCannotConnectToIsReachedFromAPlainIpv4Socket")
bool FFlockRelayConnectRefusedTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	FFlockUdpSockets::bDualStackConnectRefusedForTesting = true;
	F.Relay->AnswersOnlyPortsHeard = TSet<int32>({ 3 });
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.4;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("each search socket fell back to IPv4"), Opened->GetSocketProtocolForTesting() == FNetworkProtocolTypes::IPv4);
	TestTrue(TEXT("open"), Opened->IsOpen());
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayNoIpv6Test, "Reach.WithoutIpv6ARelayIsReachedFromAPlainIpv4Socket")
bool FFlockRelayNoIpv6Test::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	FFlockUdpSockets::bDualStackRefusedForTesting = true;
	F.Relay->AnswersOnlyPortsHeard = TSet<int32>({ 3 });
	FFlockRelayTiming Timing = Quick();
	Timing.SocketRoundWaitSeconds = 0.4;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA, Timing);
	if (!Opened.IsValid())
	{
		return false;
	}
	TestTrue(TEXT("a platform without IPv6 gets a plain IPv4 socket"), Opened->GetSocketProtocolForTesting() == FNetworkProtocolTypes::IPv4);
	TestTrue(TEXT("open"), Opened->IsOpen());
	TestTrue(TEXT("the refusal is remembered for the launch"), FFlockUdpSockets::RemembersARefusalForTesting());
	return true;
}

// ---- addresses on IPs of their own ----

FLOCK_RELAY_TEST(FFlockRelayOtherIpTest, "Reach.APlayerOnAnotherRelayIpIsHeardOnlyOnceTheHostOpensToIt")
bool FFlockRelayOtherIpTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	F.Relay->RelayIps.Add(OtherRelayIp);
	const TSharedPtr<FFlockRelayConnection> Host = F.Opened(*this, RelayPlayerA);
	const TSharedPtr<FFlockRelayConnection> Player = F.Opened(*this, RelayPlayerB);
	if (!Host.IsValid() || !Player.IsValid())
	{
		return false;
	}
	TestNotEqual(TEXT("precondition: an IP each"), Host->GetAddress().Ip(), Player->GetAddress().Ip());
	const TSharedRef<FOpenedTo> HostOpen = F.OpenTo(Host, Host->GetAddress());
	const TSharedRef<FOpenedTo> PlayerOpen = F.OpenTo(Player, Host->GetAddress());
	F.Until([&]() { return HostOpen->bAnswered && PlayerOpen->bAnswered; });
	const TArray<uint8> Before = Bytes(TEXT("before"));
	TestTrue(TEXT("sent"), Player->Send(Host->GetAddress(), Before.GetData(), Before.Num()));
	TestFalse(TEXT("not heard before the host opened to the player's relay IP"), F.Arrives(Host, TEXT("before"), 0.6));
	const TSharedRef<FOpenedTo> Opened = F.OpenTo(Host, Player->GetAddress());
	F.Until([&]() { return Opened->bAnswered; });
	TestTrue(TEXT("the host opened to the player's relay IP"), Opened->Result.bSuccess);
	const TArray<uint8> After = Bytes(TEXT("after"));
	TestTrue(TEXT("sent"), Player->Send(Host->GetAddress(), After.GetData(), After.Num()));
	TestTrue(TEXT("heard once the host opened to it"), F.Arrives(Host, TEXT("after"), 3.0));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayAskedTwiceTest, "Reach.OpeningToOneIpAskedTwiceAtOnceSendsOneRequest")
bool FFlockRelayAskedTwiceTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	F.Relay->AnswerDelaySeconds = 0.3;
	const FFlockRelayPeer Player(0xCB007133, 5000);
	const TSharedRef<FOpenedTo> First = F.OpenTo(Opened, Player);
	const TSharedRef<FOpenedTo> Second = F.OpenTo(Opened, Player);
	TestEqual(TEXT("one opening asked"), Opened->GetOpeningsAskedForTesting(), 1);
	F.Until([&]() { return First->bAnswered && Second->bAnswered; });
	TestTrue(TEXT("both opened"), First->Result.bSuccess && Second->Result.bSuccess);
	TestEqual(TEXT("one request for both callers"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodCreatePermission).Num(), 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayMostOpeningsTest, "Reach.ARelayAddressOpenedToTheMostIpsRefusesAnotherWithoutAsking")
bool FFlockRelayMostOpeningsTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	FFlockRelayConnection::MostOpeningsForTesting = 2;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	const TSharedRef<FOpenedTo> First = F.OpenTo(Opened, FFlockRelayPeer(0xCB007141, 5000));
	const TSharedRef<FOpenedTo> Second = F.OpenTo(Opened, FFlockRelayPeer(0xCB007142, 5000));
	F.Until([&]() { return First->bAnswered && Second->bAnswered; });
	TestTrue(TEXT("precondition: the first two opened"), First->Result.bSuccess && Second->Result.bSuccess);
	const TSharedRef<FOpenedTo> Third = F.OpenTo(Opened, FFlockRelayPeer(0xCB007143, 5000));
	TestTrue(TEXT("a third IP past the most is refused at once"), Third->bAnswered && !Third->Result.bSuccess);
	TestEqualSensitive(TEXT("as open refused"), Third->Result.Failure.Reason, FString(FlockRelayFailureReasons::OpenRefused));
	F.RealSeconds(0.2);
	TestEqual(TEXT("the third IP was never asked for"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodCreatePermission).Num(), 2);
	TestTrue(TEXT("remembered, so a host tells it once"), Opened->RefusedToOpenTo(FFlockRelayPeer(0xCB007143, 5000)));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayRefusedAgainTest, "Reach.AnIpTheRelayRefusedIsRefusedAgainWithoutAsking")
bool FFlockRelayRefusedAgainTest::RunTest(const FString& Parameters)
{
	FRelayFixture F;
	const TSharedPtr<FFlockRelayConnection> Opened = F.Opened(*this, RelayPlayerA);
	if (!Opened.IsValid())
	{
		return false;
	}
	F.Relay->RefusedOpenings.Add(TEXT("203.0.113.99"));
	const FFlockRelayPeer Refused(0xCB007163, 5000);
	const TSharedRef<FOpenedTo> First = F.OpenTo(Opened, Refused);
	F.Until([&]() { return First->bAnswered; });
	const TSharedRef<FOpenedTo> Second = F.OpenTo(Opened, Refused);
	TestTrue(TEXT("refused again at once"), Second->bAnswered);
	TestEqualSensitive(TEXT("first refused"), First->Result.Failure.Reason, FString(FlockRelayFailureReasons::OpenRefused));
	TestEqualSensitive(TEXT("second refused"), Second->Result.Failure.Reason, FString(FlockRelayFailureReasons::OpenRefused));
	TestEqual(TEXT("asked once"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodCreatePermission).Num(), 1);
	return true;
}

// ---- through a session ----

namespace FlockRelayTestHelpers
{
	using namespace FlockConnectingTestHelpers;

	/** The session stack over the fake session server, whose seats draw logins for a fake relay on loopback. */
	struct FSessionRelayFixture : FConnectingFixture
	{
		TUniquePtr<FFlockFakeRelayServer> Relay = MakeUnique<FFlockFakeRelayServer>();

		explicit FSessionRelayFixture(int32 MaxRetries = 0)
			: FConnectingFixture(MaxRetries)
		{
			Server->RelayUrls.Add(Relay->Url());
			Sessions().SetRelayTimingForTesting(Quick());
		}

		TSharedRef<FOpening> OpenRelay(UFlockMultiplayerSession* Of, const FString& OnServer = FString(), FFlockRequestHandle* OutHandle = nullptr)
		{
			const TSharedRef<FOpening> Answer = MakeShared<FOpening>();
			const FFlockRequestHandle Handle = Sessions().OpenRelay(Of, OnServer, Into(Answer));
			if (OutHandle != nullptr)
			{
				*OutHandle = Handle;
			}
			return Answer;
		}

		/** Runs frames, a millisecond apart, until Done or Seconds pass. */
		bool RelayFramesUntil(TFunctionRef<bool()> Done, double Seconds = 10.0)
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

		void RealSeconds(double Seconds) { RelayFramesUntil([]() { return false; }, Seconds); }

		TArray<FFlockHttpRequest> Logins() const { return Server->RequestsTo(ERoute::RelayCredentials); }
	};
}

FLOCK_RELAY_TEST(FFlockSessionRelayNamesTest, "Session.OpenRelayNamesTheSessionAndReservesWithFlocksLogin")
bool FFlockSessionRelayNamesTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	if (!TestTrue(FString::Printf(TEXT("opened (%s)"), *Opening->Result.Failure.Error.Message), Opening->Result.bSuccess))
	{
		return false;
	}
	const TArray<FFlockHttpRequest> Asked = F.Logins();
	if (TestEqual(TEXT("one request for logins"), Asked.Num(), 1))
	{
		TestEqualSensitive(TEXT("the logins name the session"), F.Field(BodyOf(Asked[0]), TEXT("session_id")), Session->GetId());
		TestEqualSensitive(TEXT("asked as the signed-in player"), FFlockFakePartyServer::PlayerOf(Asked[0]), PlayerA);
	}
	TestTrue(TEXT("the session holds its relay"), F.Sessions().GetRelayForTesting() == Opening->Result.Value);
	TestEqual(TEXT("one reservation"), F.Relay->Reservations(), 1);
	TestTrue(TEXT("answered on the game thread"), Opening->AnsweredOnThread == GGameThreadId);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayStunTest, "Session.OpenRelayKeepsTheStunServersFlockListed")
bool FFlockSessionRelayStunTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.Server->StunUrls.Reset();
	F.Server->StunUrls.Add(TEXT("stun:127.0.0.1:3479"));
	F.Server->StunUrls.Add(TEXT("stuns:127.0.0.1:5349"));
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	if (!TestTrue(TEXT("opened"), Opening->Result.bSuccess))
	{
		return false;
	}
	const TArray<FFlockRelayPeer> Stun = Opening->Result.Value->GetStunServers();
	TestTrue(TEXT("the UDP STUN server Flock listed, looked up"), Stun.Num() == 1 && Stun[0].ToString() == TEXT("127.0.0.1:3479"));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayStunNotFoundTest, "Session.OpenRelayAStunServerWhoseNameCannotBeLookedUpIsLeftOutAndTheRelayOpens")
bool FFlockSessionRelayStunNotFoundTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.Server->StunUrls.Reset();
	F.Server->StunUrls.Add(TEXT("stun:refused.stun.test:3478"));
	F.Server->StunUrls.Add(TEXT("stun:127.0.0.1:3479"));
	FFlockRelayTiming Timing = Quick();
	// The resolver finds nothing for the name.
	Timing.NameLookUpForTesting = [](const FString&, TFunction<void(const FString&)> Answer) { Answer(FString()); };
	F.Sessions().SetRelayTimingForTesting(Timing);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	if (!TestTrue(FString::Printf(TEXT("opened (%s)"), *Opening->Result.Failure.Error.Message), Opening->Result.bSuccess))
	{
		return false;
	}
	const TArray<FFlockRelayPeer> Stun = Opening->Result.Value->GetStunServers();
	TestTrue(TEXT("the one that could not be looked up left out"), Stun.Num() == 1 && Stun[0].ToString() == TEXT("127.0.0.1:3479"));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelaySharedTest, "Session.OpenRelayTwoCallersShareOneReservation")
bool FFlockSessionRelaySharedTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> First = F.OpenRelay(Session);
	const TSharedRef<FOpening> Second = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return First->bAnswered && Second->bAnswered; });
	TestTrue(TEXT("both opened, on one relay"), First->Result.bSuccess && Second->Result.bSuccess && First->Result.Value == Second->Result.Value);
	TestEqual(TEXT("one request for logins"), F.Logins().Num(), 1);
	TestEqual(TEXT("one reservation"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; }).Num(), 1);
	const TSharedRef<FOpening> Again = F.OpenRelay(Session);
	TestTrue(TEXT("an open relay is handed back as it is, at once"), Again->bAnswered && Again->Result.Value == First->Result.Value);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayHostsServerTest, "Session.OpenRelayOnTheHostsServerReplacesARelayOpenOnAnotherAndKeepsOneOnIt")
bool FFlockSessionRelayHostsServerTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	FFlockFakeRelayServer Hosts;
	F.Server->RelayUrls.Add(Hosts.Url());
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> AnyServer = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return AnyServer->bAnswered; });
	TestEqual(TEXT("precondition: on the first server Flock lists"), F.Relay->Reservations(), 1);

	const FString HostsServer = FString::Printf(TEXT("127.0.0.1:%d"), Hosts.GetPort());
	const TSharedRef<FOpening> OnTheHosts = F.OpenRelay(Session, HostsServer);
	F.RelayFramesUntil([&]() { return OnTheHosts->bAnswered; });
	if (!TestTrue(FString::Printf(TEXT("opened on the host's server (%s)"), *OnTheHosts->Result.Failure.Error.Message), OnTheHosts->Result.bSuccess))
	{
		return false;
	}
	TestEqualSensitive(TEXT("on the host's server"), OnTheHosts->Result.Value->GetServer(), HostsServer);
	TestTrue(TEXT("the relay on the other server made way"), AnyServer->Result.Value.IsValid() && AnyServer->Result.Value->HasStopped());
	F.RelayFramesUntil([&]() { return F.Relay->Releases() == 1; }, 3.0);
	TestEqual(TEXT("and was given back"), F.Relay->Releases(), 1);

	const TSharedRef<FOpening> Again = F.OpenRelay(Session, HostsServer.ToUpper());
	TestTrue(TEXT("one already on the host's server is handed back as it is"), Again->bAnswered && Again->Result.Value == OnTheHosts->Result.Value);
	const TSharedRef<FOpening> AnyAgain = F.OpenRelay(Session);
	TestTrue(TEXT("any server will do for a caller who names none"), AnyAgain->bAnswered && AnyAgain->Result.Value == OnTheHosts->Result.Value);
	TestEqual(TEXT("one reservation on the host's server"), Hosts.Reservations(), 1);
	F.Sessions().GetRelayForTesting()->Close();
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayHostsServerWhileOpeningTest, "Session.OpenRelayOnTheHostsServerWaitsForAnOpeningOnAnotherThenOpensOnIt")
bool FFlockSessionRelayHostsServerWhileOpeningTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	FFlockFakeRelayServer Hosts;
	F.Server->RelayUrls.Add(Hosts.Url());
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Relay->bHoldReservationAnswers = true;
	const TSharedRef<FOpening> AnyServer = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; }).Num() > 0; });
	const FString HostsServer = FString::Printf(TEXT("127.0.0.1:%d"), Hosts.GetPort());
	const TSharedRef<FOpening> OnTheHosts = F.OpenRelay(Session, HostsServer);
	F.RealSeconds(0.2);
	TestFalse(TEXT("waits while the other opening is under way"), OnTheHosts->bAnswered);
	F.Relay->ReleaseHeldAnswers();
	F.RelayFramesUntil([&]() { return OnTheHosts->bAnswered; });
	TestTrue(TEXT("then opened on the host's server"), OnTheHosts->Result.bSuccess && OnTheHosts->Result.Value->GetServer() == HostsServer);
	TestTrue(TEXT("the first caller was answered too"), AnyServer->bAnswered);
	F.RelayFramesUntil([&]() { return F.Relay->Reservations() == 0; }, 3.0);
	TestEqual(TEXT("the one on the other server was given back"), F.Relay->Reservations(), 0);
	F.Sessions().GetRelayForTesting()->Close();
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayGiveUpTest, "Session.OpenRelayACallerThatGivesUpLeavesTheOpeningToTheOthers")
bool FFlockSessionRelayGiveUpTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Relay->bHoldReservationAnswers = true;
	FFlockRequestHandle GivingUpHandle;
	const TSharedRef<FOpening> GivingUp = F.OpenRelay(Session, FString(), &GivingUpHandle);
	const TSharedRef<FOpening> Staying = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; }).Num() > 0; });
	GivingUpHandle.Cancel();
	TestTrue(TEXT("the caller stopped waiting, at once"), GivingUp->bAnswered && GivingUp->Result.Failure.IsCancelled());
	F.Relay->ReleaseHeldAnswers();
	F.RelayFramesUntil([&]() { return Staying->bAnswered; });
	TestTrue(TEXT("the other caller's relay opened"), Staying->Result.bSuccess && Staying->Result.Value->IsOpen());
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayNextServerTest, "Session.OpenRelayTheFirstRelayDoesNotAnswerOpensOnTheNext")
bool FFlockSessionRelayNextServerTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	FFlockFakeRelayServer Silent;
	Silent.bSilent = true;
	F.Server->RelayUrls.Insert(Silent.Url(), 0);
	F.Server->StunUrls.Reset();
	F.Server->StunUrls.Add(TEXT("stun:stun.counted.test:3478"));
	FFlockRelayTiming Timing = Quick();
	Timing.AnswerWaitSeconds = 1.0;
	Timing.SocketRoundWaitSeconds = 0.2;
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> StunLookUps = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	Timing.NameLookUpForTesting = [StunLookUps](const FString& Host, TFunction<void(const FString&)> Answer)
	{
		*StunLookUps += Host == TEXT("stun.counted.test") ? 1 : 0;
		Answer(TEXT("127.0.0.1"));
	};
	F.Sessions().SetRelayTimingForTesting(Timing);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestTrue(FString::Printf(TEXT("opened (%s)"), *Opening->Result.Failure.Error.Message), Opening->Result.bSuccess);
	TestTrue(TEXT("precondition: the first relay was asked"), Silent.SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num() > 0);
	TestEqual(TEXT("reserved on the next relay Flock lists"), F.Relay->Reservations(), 1);
	TestEqual(TEXT("the STUN server was looked up once for both relays tried"), StunLookUps->load(), 1);
	TestTrue(TEXT("and the relay that opened has it"), Opening->Result.bSuccess && Opening->Result.Value->GetStunServers().Num() == 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayFirstReasonTest, "Session.OpenRelayNoRelayOpensTellsTheFirstRelaysReason")
bool FFlockSessionRelayFirstReasonTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	FFlockFakeRelayServer Silent;
	Silent.bSilent = true;
	F.Server->RelayUrls.Insert(Silent.Url(), 0);
	F.Relay->Password = TEXT("not-what-flock-minted");
	FFlockRelayTiming Timing = Quick();
	Timing.AnswerWaitSeconds = 1.0;
	Timing.SocketRoundWaitSeconds = 0.2;
	F.Sessions().SetRelayTimingForTesting(Timing);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqual(TEXT("precondition: the next relay was asked too"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; }).Num(), 1);
	TestEqualSensitive(TEXT("Flock's main relay's reason, not the next one's wrong login"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Unreachable));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayPausedTest, "Session.OpenRelayRelayPausedFailsPaused")
bool FFlockSessionRelayPausedTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.Server->bRelayPaused = true;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("paused"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::Paused));
	TestEqual(TEXT("nothing asked of the relay"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num(), 0);
	TestFalse(TEXT("the failure says how to fix it"), Opening->Result.Failure.Error.Hint.IsEmpty());
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayNotOfferedTest, "Session.OpenRelayNoRelayListedFailsNotOffered")
bool FFlockSessionRelayNotOfferedTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.Server->RelayUrls.Reset();
	F.Server->RelayUrls.Add(TEXT("turn:relay.test:3479?transport=tcp"));
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("a relay over TCP alone is none this client can use"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::NotOffered));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayServerNotListedTest, "Session.OpenRelayOnAServerFlockDoesNotListFailsServerNotListed")
bool FFlockSessionRelayServerNotListedTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session, TEXT("another.relay.test:3478"));
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("server not listed"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::ServerNotListed));
	TestEqual(TEXT("nothing asked of the relay"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayRateTest, "Session.OpenRelayTooManyLoginsFailsTooManyLogins")
bool FFlockSessionRelayRateTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::RelayCredentials, FFlockFakeSessionServer::Refused(429, TEXT("multiplayer.mint_rate_limited")));
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("too many logins"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::TooManyLogins));
	TestEqualSensitive(TEXT("Flock's own code is kept"), Opening->Result.Failure.Error.Code, FString(TEXT("multiplayer.mint_rate_limited")));
	TestTrue(TEXT("says the minute's allowance is used, not the seat's"), Opening->Result.Failure.Error.Message.Contains(TEXT("minute")));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelaySeatTest, "Session.OpenRelayTheSeatDrewTheMostLoginsFailsTooManyLoginsNamingTheSeat")
bool FFlockSessionRelaySeatTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::RelayCredentials, FFlockFakeSessionServer::Refused(429, TEXT("multiplayer.seat_mint_limit")));
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("too many logins"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::TooManyLogins));
	TestTrue(TEXT("says the seat's allowance is used, not the minute's"), Opening->Result.Failure.Error.Message.Contains(TEXT("seat")));
	TestTrue(TEXT("its fix names the seat's allowance"), Opening->Result.Failure.Error.Hint.Contains(TEXT("seat")));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayFlockUnreachableTest, "Session.OpenRelayFlockDoesNotAnswerFailsFlockUnreachable")
bool FFlockSessionRelayFlockUnreachableTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	FFlockHttpResponse Unreachable;
	Unreachable.Result = EFlockHttpResult::ConnectionError;
	F.Server->AnswerNext(ERoute::RelayCredentials, Unreachable);
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("Flock unreachable"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::FlockUnreachable));
	return true;
}

FLOCK_RELAY_TEST(FFlockRelayFixesTest, "EveryFailureReasonHasAFixAndATypeOfItsOwn")
bool FFlockRelayFixesTest::RunTest(const FString& Parameters)
{
	const FString General = FlockRelayFailureReasons::FixFor(TEXT("relay_not_a_reason"));
	for (const TCHAR* Each : { FlockRelayFailureReasons::NotOffered, FlockRelayFailureReasons::ServerNotListed, FlockRelayFailureReasons::Paused,
		FlockRelayFailureReasons::TooManyLogins, FlockRelayFailureReasons::NotInSession, FlockRelayFailureReasons::FlockUnreachable,
		FlockRelayFailureReasons::LoginsRefused, FlockRelayFailureReasons::Unreachable, FlockRelayFailureReasons::WrongLogin,
		FlockRelayFailureReasons::Full, FlockRelayFailureReasons::OpenRefused, FlockRelayFailureReasons::Refused, FlockRelayFailureReasons::Lost,
		FlockRelayFailureReasons::Closed, FlockRelayFailureReasons::NoThread })
	{
		const FFlockRelayFailure Failure = FFlockRelayFailure::Of(Each, TEXT("why"));
		TestFalse(*FString::Printf(TEXT("%s has a fix of its own"), Each), Failure.Error.Hint == General || Failure.Error.Hint.IsEmpty());
		TestEqualSensitive(*FString::Printf(TEXT("%s is told as opening the relay"), Each), Failure.Error.Operation, FString(TEXT("Open the relay")));
		const bool bOutOfReach = FCString::Strcmp(Each, FlockRelayFailureReasons::Unreachable) == 0
			|| FCString::Strcmp(Each, FlockRelayFailureReasons::FlockUnreachable) == 0;
		TestEqual(*FString::Printf(TEXT("%s is a %s failure"), Each, bOutOfReach ? TEXT("connection") : TEXT("network")),
			static_cast<int32>(Failure.Error.Type), static_cast<int32>(bOutOfReach ? EFlockErrorType::Connection : EFlockErrorType::Network));
	}
	TestTrue(TEXT("a reason in other letters is not the SDK's"), FlockRelayFailureReasons::FixFor(TEXT("RELAY_FULL")) == General);
	TestEqual(TEXT("nor its type"), static_cast<int32>(FFlockRelayFailure::Of(TEXT("RELAY_UNREACHABLE"), TEXT("why")).Error.Type),
		static_cast<int32>(EFlockErrorType::Network));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayLoginsRefusedTest, "Session.OpenRelayFlockRefusingForAnotherCauseFailsLoginsRefusedWithItsFix")
bool FFlockSessionRelayLoginsRefusedTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::RelayCredentials, FFlockFakeSessionServer::Refused(500, TEXT("server.error")));
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("logins refused"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::LoginsRefused));
	TestEqualSensitive(TEXT("Flock's own code is kept"), Opening->Result.Failure.Error.Code, FString(TEXT("server.error")));
	TestFalse(TEXT("with a fix"), Opening->Result.Failure.Error.Hint.IsEmpty());
	TestEqual(TEXT("nothing was asked of the relay"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayNotSeatedTest, "Session.OpenRelaySessionFlockNoLongerSeatsThePlayerInFailsNotInSession")
bool FFlockSessionRelayNotSeatedTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->Ends(F.Server->ById(Session->GetId()).ToSharedRef());
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestEqualSensitive(TEXT("not in the session"), Opening->Result.Failure.Reason, FString(FlockRelayFailureReasons::NotInSession));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayEndedTest, "Session.OpenRelayOnAnEndedSessionIsRefusedWithoutAskingFlock")
bool FFlockSessionRelayEndedTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Session->Leave(Into(Left));
	F.FramesUntil([&]() { return Left->bAnswered; });
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	TestTrue(TEXT("refused at once"), Opening->bAnswered && !Opening->Result.bSuccess);
	TestEqual(TEXT("refused as for any call on an ended session"), static_cast<int32>(Opening->Result.Failure.Error.Type), static_cast<int32>(EFlockErrorType::Validation));
	TestEqual(TEXT("no logins asked for"), F.Logins().Num(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayEndTest, "Session.SessionEndGivesTheRelayBackBeforeEndedIsRaised")
bool FFlockSessionRelayEndTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	if (!TestTrue(TEXT("precondition: opened"), Opening->Result.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FFlockRelayConnection> Relay = Opening->Result.Value;
	UFlockSessionTestListener* Listener = NewObject<UFlockSessionTestListener>();
	TStrongObjectPtr<UFlockSessionTestListener> KeepListener(Listener);
	bool bStoppedWhenEnded = false;
	Listener->AlsoInHandler = [Relay, Listener, &bStoppedWhenEnded]()
	{
		if (Listener->Ended.Num() == 1)
		{
			bStoppedWhenEnded = Relay->HasStopped();
		}
	};
	Listener->Listen(Session);
	const TSharedRef<FSessionAnswer> Ended = MakeShared<FSessionAnswer>();
	Session->End(Into(Ended));
	F.FramesUntil([&]() { return Ended->bAnswered; });
	TestTrue(TEXT("the session ended"), Session->HasEnded());
	TestTrue(TEXT("the relay was closed before the game heard of the end"), bStoppedWhenEnded);
	F.RelayFramesUntil([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Releases(), 1);
	TestFalse(TEXT("the session holds no relay"), F.Sessions().GetRelayForTesting().IsValid());
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayEndWhileOpeningTest, "Session.SessionEndWhileTheRelayOpensCancelsItAndGivesItBack")
bool FFlockSessionRelayEndWhileOpeningTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Relay->bHoldReservationAnswers = true;
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; }).Num() > 0; });
	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Session->Leave(Into(Left));
	F.FramesUntil([&]() { return Left->bAnswered; });
	TestTrue(TEXT("the opening ended as cancelled"), Opening->bAnswered && Opening->Result.Failure.IsCancelled());
	F.RelayFramesUntil([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	F.Relay->ReleaseHeldAnswers();
	F.RealSeconds(0.2);
	TestEqual(TEXT("answered once"), Opening->Times, 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayAskedAgainTest, "Session.ACallerAskingAgainAsTheSessionEndsOpensNothing")
bool FFlockSessionRelayAskedAgainTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Relay->bHoldReservationAnswers = true;
	// A caller that asks again from its answer, as a game retrying its networking would.
	const TSharedRef<FOpening> Again = MakeShared<FOpening>();
	bool bAskedAgain = false;
	FFlockMultiplayerSessions& Sessions = F.Sessions();
	Sessions.OpenRelay(Session, FString(), [&Sessions, Session, Again, &bAskedAgain](FOpenedResult)
	{
		bAskedAgain = true;
		Sessions.OpenRelay(Session, FString(), Into(Again));
	});
	F.RelayFramesUntil([&]() { return F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).FilterByPredicate([](const FSeen& Seen) { return Seen.bSigned; }).Num() > 0; });
	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Session->Leave(Into(Left));
	F.FramesUntil([&]() { return Left->bAnswered; });
	TestTrue(TEXT("precondition: the caller asked again from the end's answer"), bAskedAgain);
	TestTrue(TEXT("refused at once"), Again->bAnswered && !Again->Result.bSuccess);
	TestEqualSensitive(TEXT("as out of the session"), Again->Result.Failure.Reason, FString(FlockRelayFailureReasons::NotInSession));
	TestFalse(TEXT("no relay is left for the ended session"), F.Sessions().GetRelayForTesting().IsValid());
	TestEqual(TEXT("no more logins asked for"), F.Logins().Num(), 1);
	F.Relay->ReleaseHeldAnswers();
	F.RelayFramesUntil([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("no address kept"), F.Relay->Reservations(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayOtherSignInTest, "Session.LoginsLandingAfterAnotherPlayerSignedInOpenNothing")
bool FFlockSessionRelayOtherSignInTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Held = F.Server->HoldNext(ERoute::RelayCredentials);
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	// Another player signs in while Flock answers, before the frame that ends the first player's session.
	F.SignIn(PlayerB);
	Held->Release();
	TestTrue(TEXT("the opening ended as cancelled"), Opening->bAnswered && Opening->Result.Failure.IsCancelled());
	F.RealSeconds(0.3);
	TestEqual(TEXT("nothing was asked of the relay with the first player's login"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayLoginsInFlightTest, "Session.SessionEndWhileFlockAnswersTheLoginsOpensNothing")
bool FFlockSessionRelayLoginsInFlightTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FFlockFakePartyServer::FHeld> Held = F.Server->HoldNext(ERoute::RelayCredentials);
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Session->Leave(Into(Left));
	F.FramesUntil([&]() { return Left->bAnswered; });
	TestTrue(TEXT("the opening ended as cancelled"), Opening->bAnswered && Opening->Result.Failure.IsCancelled());
	Held->Release();
	F.RealSeconds(0.5);
	TestEqual(TEXT("nothing was asked of the relay"), F.Relay->SeenRequests(FFlockFakeRelayServer::MethodAllocate).Num(), 0);
	TestEqual(TEXT("answered once"), Opening->Times, 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayLoginsRetryTest, "Session.SessionEndStopsTheLoginsRequestTryingAgain")
bool FFlockSessionRelayLoginsRetryTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F(/*MaxRetries*/ 1);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::RelayCredentials, FFlockFakeSessionServer::Refused(503, TEXT("server.unavailable")));
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	// Flock's 503 has landed and its retry waits for the core ticker, which no frame runs: the session ends meanwhile.
	const TSharedRef<FSessionAnswer> Left = MakeShared<FSessionAnswer>();
	Session->Leave(Into(Left));
	F.FramesUntil([&]() { return Left->bAnswered; });
	TestTrue(TEXT("the opening ended as cancelled"), Opening->bAnswered && Opening->Result.Failure.IsCancelled());
	FlockTestRetries::Pump();
	TestEqual(TEXT("the retry did not go out for the ended session"), F.Logins().Num(), 1);
	TestEqual(TEXT("answered once"), Opening->Times, 1);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayLoginsRetryControlTest, "Session.LoginsFlockFailedToGiveAreAskedForAgain")
bool FFlockSessionRelayLoginsRetryControlTest::RunTest(const FString& Parameters)
{
	// The control for the test above: with the session still on, the same 503 is tried again and the relay opens.
	FSessionRelayFixture F(/*MaxRetries*/ 1);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	F.Server->AnswerNext(ERoute::RelayCredentials, FFlockFakeSessionServer::Refused(503, TEXT("server.unavailable")));
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	FlockTestRetries::Pump();
	TestEqual(TEXT("asked again"), F.Logins().Num(), 2);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	TestTrue(TEXT("and the relay opened"), Opening->bAnswered && Opening->Result.bSuccess);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelaySignOutTest, "Session.SignOutGivesTheRelayBack")
bool FFlockSessionRelaySignOutTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	if (!TestTrue(TEXT("precondition: opened"), Opening->Result.bSuccess))
	{
		return false;
	}
	F.Session->ClearTokens();
	F.RelayFramesUntil([&]() { return Opening->Result.Value->HasStopped(); }, 2.0);
	TestTrue(TEXT("the relay closed once the sign-in ended"), Opening->Result.Value->HasStopped());
	TestEqualSensitive(TEXT("the session ended signed out"), Session->GetEndReason(), FString(FlockMultiplayerSessionEndReasons::SignedOut));
	F.RelayFramesUntil([&]() { return F.Relay->Reservations() == 0; }, 2.0);
	TestEqual(TEXT("the address was given back"), F.Relay->Reservations(), 0);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayShutdownTest, "Session.FlockShuttingDownGivesTheRelayBackWithoutAWord")
bool FFlockSessionRelayShutdownTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> Opening = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Opening->bAnswered; });
	if (!TestTrue(TEXT("precondition: opened"), Opening->Result.bSuccess))
	{
		return false;
	}
	F.Provider->Shutdown();
	TestTrue(TEXT("closed at once"), Opening->Result.Value->HasStopped());
	F.RealSeconds(0.3);
	TestEqual(TEXT("the address was given back"), F.Relay->Releases(), 1);
	TestFalse(TEXT("not reported as lost"), FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("was lost")));
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayAgainTest, "Session.OpenRelayAfterTheRelayStoppedOpensANewOne")
bool FFlockSessionRelayAgainTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> First = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return First->bAnswered; });
	if (!TestTrue(TEXT("precondition: opened"), First->Result.bSuccess))
	{
		return false;
	}
	First->Result.Value->Close();
	const TSharedRef<FOpening> Second = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Second->bAnswered; });
	TestTrue(TEXT("a new relay opened"), Second->Result.bSuccess && Second->Result.Value != First->Result.Value && Second->Result.Value->IsOpen());
	TestTrue(TEXT("held by the session"), F.Sessions().GetRelayForTesting() == Second->Result.Value);
	TestEqual(TEXT("logins asked once for each"), F.Logins().Num(), 2);
	return true;
}

FLOCK_RELAY_TEST(FFlockSessionRelayLostTest, "Session.ARelayLostIsSaidOnceWithItsFixAndTheNextOpenMakesANewOne")
bool FFlockSessionRelayLostTest::RunTest(const FString& Parameters)
{
	FSessionRelayFixture F;
	FFlockRelayTiming Timing = Quick();
	Timing.LongestBetweenReservationRenewalsSeconds = 0.5;
	F.Sessions().SetRelayTimingForTesting(Timing);
	F.SignIn(PlayerA);
	UFlockMultiplayerSession* Session = F.Hosted(*this);
	if (Session == nullptr)
	{
		return false;
	}
	const TSharedRef<FOpening> First = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return First->bAnswered; });
	if (!TestTrue(TEXT("precondition: opened"), First->Result.bSuccess))
	{
		return false;
	}
	F.Relay->ForgetEveryReservation();
	// The session owner's frames hand the game what the relay's thread has: the loss is said from them.
	F.RelayFramesUntil([&]() { return FFlockRecordingLogger::AnyContains(F.Log->Warnings, TEXT("was lost")); }, 3.0);
	TestEqual(TEXT("said once, through the frames a game runs"), F.Log->Warnings.FilterByPredicate([](const FString& Line) { return Line.Contains(TEXT("was lost")); }).Num(), 1);
	const TSharedRef<FOpening> Second = F.OpenRelay(Session);
	F.RelayFramesUntil([&]() { return Second->bAnswered; });
	TestTrue(TEXT("a new relay opened"), Second->Result.bSuccess && Second->Result.Value != First->Result.Value);
	return true;
}

#endif
