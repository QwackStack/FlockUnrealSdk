// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Multiplayer/Relay/FlockRelayRenewals.h"
#include "Multiplayer/Relay/FlockRelayRequests.h"
#include "Multiplayer/Relay/FlockRelaySocket.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include <atomic>

int32 FFlockRelayConnection::MostOpeningsForTesting = 0;
bool FFlockRelayConnection::bThreadRefusedForTesting = false;

namespace
{
	/** How long the relay's thread waits on its socket at a time, so a renewal or a request handed over never waits longer. */
	constexpr double LongestPollSeconds = 0.02;
	/** The most packets read in one turn, so a flood cannot hold the renewals back. */
	constexpr int32 MostReadsATurn = 512;

	enum class EState : int32 { Opening, Open, Closed, Failed };

	/** What the relay's thread has for the game thread, taken in Tick. */
	struct FForTheGame
	{
		enum class EKind : uint8 { Opened, Stopped, OpeningAnswered, Warning };
		EKind Kind = EKind::Warning;
		FString Reason;
		FString Message;
		bool bWasOpen = false;
		uint32 Ip = 0;
		int32 Code = 0;
	};
}

/**
 * The part of a relay connection the relay's thread shares: its socket, requests, packets and renewals, and the state both
 * sides read. Kept alive by the thread until the thread ends, so a connection its owner dropped still gives its address back.
 */
class FFlockRelayCore : public FRunnable
{
public:
	// The callbacks below capture this: they are held by members of this core and run only by its own thread, so none can
	// outlive it, and its owner's teardown does not end it (the thread keeps it until it ends).
	FFlockRelayCore(const FFlockRelayLogin& InLogin, const TArray<FFlockRelayStunLookUp>& InStunLookUps, const FFlockRelayTiming& InTiming)
		: Login(InLogin)
		, StunLookUps(InStunLookUps)
		, Timing(InTiming)
		, Requests(Login, Socket, Timing)
		, Packets(Socket)
		, Renewals(Requests, Packets, Timing,
			[this](const FString& Message) { Hand({ FForTheGame::EKind::Warning, FString(), Message }); },
			[this](const FString& What) { Lose(What); })
	{
	}

	// ---- the relay's thread ----

	virtual uint32 Run() override
	{
		ThreadId = FPlatformTLS::GetCurrentThreadId();
		Received.SetNumUninitialized(65536);
		OpenTheSocket();
		Serve();
		Socket.Close();
		bThreadDone = true;
		return 0;
	}

	// ---- both sides ----

	/** Stops the connection: the release goes out from this thread at once, so it is sent even while the game quits; the relay's thread closes the socket once it is answered, or after a moment, sending it again if the relay's nonce went stale. */
	void StopConnection(EState FinalState, const FString& Reason, const FString& Message)
	{
		bool bWasStopping = false;
		if (!bStopping.compare_exchange_strong(bWasStopping, true))
		{
			return;
		}
		// The reason first, so a reader that sees the state stopped finds it; swapped in one step, so an answer can no longer open it.
		{
			FScopeLock Locked(&Lock);
			FailureReason = Reason;
		}
		const bool bWasOpen = static_cast<EState>(State.exchange(static_cast<int32>(FinalState))) == EState::Open;
		// Sending stops before the reservation is asked about: a signed reservation is marked asked before it goes, so one the
		// relay's thread is sending now either goes before this (and is released here) or does not go at all.
		Requests.StopSending();
		if (Requests.WasReservationAsked())
		{
			CloseSocketAt = FPlatformTime::Seconds() + Timing.ReleaseAnswerWaitSeconds;
			Requests.Release();
		}
		bStopSettled = true;
		if (!Reason.IsEmpty())
		{
			Hand({ FForTheGame::EKind::Stopped, Reason, Message, bWasOpen });
		}
	}

	/** Gives up at once, sending nothing more: Flock's module is going away. */
	void QuitNow() { bQuitNow = true; }

	void HandOver(const FFlockRelayRequestRef& Request)
	{
		FScopeLock Locked(&Lock);
		HandedOver.Add(Request);
	}

	void Hand(const FForTheGame& Item) { ForTheGame.Enqueue(Item); }

	bool TakeForTheGame(FForTheGame& OutItem) { return ForTheGame.Dequeue(OutItem); }

	EState GetState() const { return static_cast<EState>(State.load()); }

	FString GetFailureReason() const
	{
		FScopeLock Locked(&Lock);
		return FailureReason;
	}

	FFlockRelayPeer GetAddress() const
	{
		FScopeLock Locked(&Lock);
		return Address;
	}

	FFlockRelayPeer GetServerEndpoint() const
	{
		FScopeLock Locked(&Lock);
		return ServerEndpoint;
	}

	TArray<FFlockRelayPeer> GetStunServers() const
	{
		FScopeLock Locked(&Lock);
		return StunServers;
	}

	const FFlockRelayLogin Login;
	/** The STUN servers Flock listed, being looked up by whoever asked for this relay. */
	const TArray<FFlockRelayStunLookUp> StunLookUps;
	const FFlockRelayTiming Timing;
	FFlockRelaySocket Socket;
	FFlockRelayRequests Requests;
	FFlockRelayPackets Packets;
	FFlockRelayRenewals Renewals;
	std::atomic<bool> bThreadDone{ false };
	std::atomic<uint32> ThreadId{ 0 };

private:
	bool IsStopping() const { return bStopping.load() || bQuitNow.load(); }

	/** Looks the relay server up, finds a socket it answers, and asks for the reservation; a failure stops the connection. */
	void OpenTheSocket()
	{
		const double Started = FPlatformTime::Seconds();
		const TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> ServerLookUp = FFlockNameLookup::Start(Login.Host, Timing.NameLookUpForTesting);
		StunDeadline = Started + Timing.NameLookUpWaitSeconds;
		while (!ServerLookUp->IsDone() && !IsStopping() && FPlatformTime::Seconds() - Started < Timing.NameLookUpWaitSeconds)
		{
			FPlatformProcess::Sleep(0.005f);
		}
		if (IsStopping())
		{
			return;
		}
		const FString ServerIp = ServerLookUp->IsDone() ? ServerLookUp->GetIpv4() : FString();
		FFlockRelayPeer Endpoint;
		if (ServerIp.IsEmpty() || !FFlockRelayPeer::TryParse(ServerIp + FString::Printf(TEXT(":%d"), Login.Port), Endpoint))
		{
			Fail(FlockRelayFailureReasons::Unreachable, FString::Printf(TEXT("The relay server's name %s could not be looked up within %.1f s"),
				*Login.Host, Timing.NameLookUpWaitSeconds));
			return;
		}
		{
			FScopeLock Locked(&Lock);
			ServerEndpoint = Endpoint;
		}
		// Reserved from a socket the server answered; when none was, from a new one as before, and the reservation's own wait judges the server.
		FSocket* Answered = FFlockRelaySocket::FindOneTheServerAnswers(ServerIp, Login.Port, Timing.SocketsTriedAtOnce, Timing.RoundsOfSocketsTried,
			Timing.SocketRoundWaitSeconds, [this]() { return IsStopping(); });
		if (Answered == nullptr && !IsStopping())
		{
			Answered = FFlockUdpSockets::OpenConnectedTo(ServerIp, Login.Port, TEXT("Flock relay"));
			if (Answered == nullptr)
			{
				Fail(FlockRelayFailureReasons::Unreachable, FString::Printf(TEXT("No way to send to the relay at %s: no UDP socket could be made "
					"and aimed at it"), *Login.ToString()));
				return;
			}
		}
		Socket.Attach(Answered);
		if (IsStopping())
		{
			return;
		}
		TArray<FFlockRelayAttribute> Attributes;
		Attributes.Emplace(FFlockRelayWire::RequestedTransportAttribute, FFlockRelayWire::UdpTransportValue());
		HandOver(MakeShared<FFlockRelayRequest, ESPMode::ThreadSafe>(FFlockRelayWire::AllocateMethod,
			[this](int32 Code, const FString& Words, const FFlockRelayMessage* Answer) { Reserved(Code, Words, Answer); }, MoveTemp(Attributes)));
	}

	void Serve()
	{
		while (!bQuitNow)
		{
			if (bStopping)
			{
				// Stopped: only the release's answer is waited for, and the socket is this thread's to close.
				if (bStopSettled && (!Requests.IsReleaseWaitingForAnswer() || FPlatformTime::Seconds() >= CloseSocketAt))
				{
					return;
				}
				if (Socket.WaitToRead(LongestPollSeconds))
				{
					ReadEverythingWaiting();
				}
				continue;
			}
			TakeHandedOver();
			if (Socket.WaitToRead(LongestPollSeconds))
			{
				ReadEverythingWaiting();
			}
			// Any step may stop the relay (a refusal read, a request given up on, a renewal lapsed); the requests send nothing
			// after that but the release.
			const double Now = FPlatformTime::Seconds();
			Requests.SendAgainOrGiveUp(Now);
			Renewals.RenewDue(Now);
			Requests.KeepAlive(Now);
			AnnounceOnceTheStunServersSettle(Now);
		}
	}

	void TakeHandedOver()
	{
		TArray<FFlockRelayRequestRef> Requested;
		{
			FScopeLock Locked(&Lock);
			Requested = MoveTemp(HandedOver);
			HandedOver.Reset();
		}
		// A new peer gets the next channel; its renewal, due at once, binds it.
		FFlockRelayPeer Peer;
		uint16 Channel = 0;
		while (Packets.TryGiveNextChannel(Peer, Channel))
		{
			Renewals.BindChannel(Peer, Channel, FPlatformTime::Seconds());
		}
		for (const FFlockRelayRequestRef& Request : Requested)
		{
			Requests.Send(Request);
		}
	}

	void ReadEverythingWaiting()
	{
		for (int32 Reads = 0; Reads < MostReadsATurn; ++Reads)
		{
			int32 Length = 0;
			// Nothing more waiting, or an earlier packet met a closed port: whether the relay answers is judged by the answer wait alone.
			if (!Socket.Receive(Received.GetData(), Received.Num(), Length))
			{
				return;
			}
			if (!Packets.TryTake(Received.GetData(), Length))
			{
				Requests.Take(Received.GetData(), Length);
			}
		}
	}

	/** The opening is answered once the STUN servers are looked up, or their wait is over. */
	void AnnounceOnceTheStunServersSettle(double Now)
	{
		if (!bReservedNotYetAnnounced)
		{
			return;
		}
		bool bAllDone = true;
		for (const FFlockRelayStunLookUp& Each : StunLookUps)
		{
			bAllDone &= Each.LookUp->IsDone();
		}
		if (!bAllDone && Now < StunDeadline)
		{
			return;
		}
		TArray<FFlockRelayPeer> Found;
		for (const FFlockRelayStunLookUp& Each : StunLookUps)
		{
			FFlockRelayPeer Server;
			// A server that cannot be looked up is left out, never failing the relay.
			if (Each.LookUp->IsDone() && FFlockRelayPeer::TryParse(Each.LookUp->GetIpv4() + FString::Printf(TEXT(":%d"), Each.Port), Server))
			{
				Found.Add(Server);
			}
		}
		{
			FScopeLock Locked(&Lock);
			StunServers = MoveTemp(Found);
		}
		bReservedNotYetAnnounced = false;
		Hand({ FForTheGame::EKind::Opened });
	}

	void Reserved(int32 Code, const FString& Words, const FFlockRelayMessage* Answer)
	{
		if (Code == 0)
		{
			FFlockRelayPeer Reservation;
			if (Answer == nullptr || !Answer->TryGetAddress(FFlockRelayWire::XorRelayedAddressAttribute, Reservation))
			{
				Fail(FlockRelayFailureReasons::Refused, FString::Printf(TEXT("The relay at %s answered with no IPv4 relay address."), *Login.ToString()));
				return;
			}
			{
				FScopeLock Locked(&Lock);
				Address = Reservation;
			}
			Renewals.KeepReservation(*Answer, FPlatformTime::Seconds());
			int32 Expected = static_cast<int32>(EState::Opening);
			if (State.compare_exchange_strong(Expected, static_cast<int32>(EState::Open)))
			{
				bReservedNotYetAnnounced = true;
			}
			return;
		}
		if (Code == FFlockRelayRequests::NoAnswer)
		{
			Fail(FlockRelayFailureReasons::Unreachable, FString::Printf(TEXT("The relay at %s did not answer within %.1f s."), *Login.ToString(),
				Timing.AnswerWaitSeconds));
		}
		else if (Code == 401 || Code == 441)
		{
			Fail(FlockRelayFailureReasons::WrongLogin, FString::Printf(TEXT("The relay at %s refused the login Flock minted (%d %s)."), *Login.ToString(),
				Code, *Words));
		}
		else if (Code == 486 || Code == 508)
		{
			Fail(FlockRelayFailureReasons::Full, FString::Printf(TEXT("The relay at %s has no room for another address (%d %s)."), *Login.ToString(),
				Code, *Words));
		}
		else
		{
			Fail(FlockRelayFailureReasons::Refused, FString::Printf(TEXT("The relay at %s refused the reservation (%d %s)."), *Login.ToString(), Code,
				*Words));
		}
	}

	void Fail(const FString& Reason, const FString& Message) { StopConnection(EState::Failed, Reason, Message); }

	void Lose(const FString& What)
	{
		StopConnection(EState::Failed, FlockRelayFailureReasons::Lost, FString::Printf(TEXT("The relay connection to %s was lost: %s."), *Login.ToString(), *What));
	}

	/** The state, the reason, the addresses and the requests handed over. */
	mutable FCriticalSection Lock;
	std::atomic<int32> State{ static_cast<int32>(EState::Opening) };
	std::atomic<bool> bStopping{ false };
	/** Set once a stop has sent what it sends; only then may the relay's thread close the socket. */
	std::atomic<bool> bStopSettled{ false };
	std::atomic<bool> bQuitNow{ false };
	/** When the socket closes whatever the release's answer. */
	std::atomic<double> CloseSocketAt{ 0.0 };
	FString FailureReason;
	FFlockRelayPeer Address;
	FFlockRelayPeer ServerEndpoint;
	TArray<FFlockRelayPeer> StunServers;
	TArray<FFlockRelayRequestRef> HandedOver;
	TQueue<FForTheGame, EQueueMode::Mpsc> ForTheGame;

	/** The relay's thread's own. */
	TArray<uint8> Received;
	double StunDeadline = 0.0;
	bool bReservedNotYetAnnounced = false;
};

namespace
{
	/**
	 * Every relay thread, so the game thread cleans each up once it ended and Flock's module waits for those still running
	 * when it goes away. A thread keeps its core alive, so a connection its owner dropped still gives its address back.
	 */
	class FFlockRelayThreads
	{
	public:
		static FFlockRelayThreads& Get()
		{
			static FFlockRelayThreads Threads;
			return Threads;
		}

		void Start(const TSharedRef<FFlockRelayCore, ESPMode::ThreadSafe>& Core)
		{
			FRunnableThread* Thread = FFlockRelayConnection::bThreadRefusedForTesting
				? nullptr
				: FRunnableThread::Create(&Core.Get(), FFlockRelayConnection::ThreadName, 0, TPri_Normal);
			if (Thread == nullptr)
			{
				// The engine runs without threads (a launch with -nothreading): the relay cannot work, and its opening says so.
				Core->bThreadDone = true;
				Core->StopConnection(EState::Failed, FlockRelayFailureReasons::NoThread,
					TEXT("No thread could be started for the relay: this launch of the game runs without threads"));
				return;
			}
			FScopeLock Locked(&Lock);
			Running.Add({ Thread, Core });
			if (!CleanUpHandle.IsValid())
			{
				CleanUpHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
				{
					FFlockRelayThreads::Get().CleanUpEnded();
					return true;
				}), 1.0f);
			}
		}

		/** Deletes the threads that ended; deleting one waits for it, which is at once. */
		void CleanUpEnded()
		{
			TArray<FEntry> Ended;
			{
				FScopeLock Locked(&Lock);
				for (int32 Index = Running.Num() - 1; Index >= 0; --Index)
				{
					if (Running[Index].Core->bThreadDone)
					{
						Ended.Add(Running[Index]);
						Running.RemoveAt(Index);
					}
				}
			}
			for (FEntry& Entry : Ended)
			{
				delete Entry.Thread;
			}
		}

		void StopAll()
		{
			TArray<FEntry> All;
			{
				FScopeLock Locked(&Lock);
				All = MoveTemp(Running);
				Running.Reset();
				if (CleanUpHandle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(CleanUpHandle);
					CleanUpHandle.Reset();
				}
			}
			for (FEntry& Entry : All)
			{
				// A relay its owner never closed gives its address back from here; one already closed sent its release then.
				Entry.Core->StopConnection(EState::Closed, FString(), FString());
				Entry.Core->QuitNow();
			}
			for (FEntry& Entry : All)
			{
				// Waits for the thread, which sees the quit within one poll.
				delete Entry.Thread;
			}
		}

		int32 Count()
		{
			CleanUpEnded();
			FScopeLock Locked(&Lock);
			return Running.Num();
		}

	private:
		struct FEntry
		{
			FRunnableThread* Thread = nullptr;
			TSharedPtr<FFlockRelayCore, ESPMode::ThreadSafe> Core;
		};

		FCriticalSection Lock;
		TArray<FEntry> Running;
		FTSTicker::FDelegateHandle CleanUpHandle;
	};
}

TSharedRef<FFlockRelayConnection> FFlockRelayConnection::Open(const FFlockRelayLogin& Login, const TArray<FFlockRelayStunLookUp>& StunServers,
	const FFlockRelayTiming& Timing, const TSharedRef<IFlockLogger>& Logger, FOnOpened OnOpened)
{
	const TSharedRef<FFlockRelayCore, ESPMode::ThreadSafe> Core = MakeShared<FFlockRelayCore, ESPMode::ThreadSafe>(Login, StunServers, Timing);
	const TSharedRef<FFlockRelayConnection> Connection = MakeShareable(new FFlockRelayConnection(Core, Logger, MoveTemp(OnOpened)));
	FFlockRelayThreads::Get().Start(Core);
	return Connection;
}

FFlockRelayConnection::FFlockRelayConnection(const TSharedRef<FFlockRelayCore, ESPMode::ThreadSafe>& InCore, const TSharedRef<IFlockLogger>& InLogger,
	FOnOpened InOnOpened)
	: Core(InCore)
	, Logger(InLogger)
	, OnOpened(MoveTemp(InOnOpened))
{
}

FFlockRelayConnection::~FFlockRelayConnection()
{
	// Its owner normally closed it; nothing is answered from here, since every caller's own owner is going away with it.
	OnOpened = nullptr;
	Waiters.Reset();
	Core->StopConnection(EState::Closed, FString(), FString());
}

void FFlockRelayConnection::Tick()
{
	// Kept alive through the answers, which may drop the owner's hold on this connection.
	const TSharedRef<FFlockRelayConnection> KeepWhileAnswering = AsShared();
	FForTheGame Item;
	while (Core->TakeForTheGame(Item))
	{
		switch (Item.Kind)
		{
		case FForTheGame::EKind::Opened:
			if (Core->GetState() == EState::Open)
			{
				AnswerOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Ok(AsShared()));
			}
			break;
		case FForTheGame::EKind::Stopped:
		{
			const FFlockRelayFailure Failure = FFlockRelayFailure::Of(Item.Reason, Item.Message);
			if (Item.bWasOpen)
			{
				Logger->LogWarning(Failure.Error.ReasonAndFix());
			}
			AnswerOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(Failure));
			Asked.Reset();
			AnswerEveryWaiter(TFlockRelayResult<bool>::Fail(Failure));
			break;
		}
		case FForTheGame::EKind::OpeningAnswered:
		{
			Asked.Remove(Item.Ip);
			if (Item.Code == 403)
			{
				Refused.Add(Item.Ip);
			}
			const FString Host = FFlockRelayPeer::IpOf(Item.Ip);
			if (Item.Code == 0)
			{
				AnswerOpeningWaiters(Item.Ip, TFlockRelayResult<bool>::Ok(true));
			}
			else if (Item.Code == FFlockRelayRequests::NoAnswer)
			{
				AnswerOpeningWaiters(Item.Ip, TFlockRelayResult<bool>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::Unreachable,
					FString::Printf(TEXT("The relay did not answer the opening to %s within %.1f s."), *Host, Core->Timing.AnswerWaitSeconds))));
			}
			else if (Item.Code == 403)
			{
				AnswerOpeningWaiters(Item.Ip, TFlockRelayResult<bool>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::OpenRefused,
					FString::Printf(TEXT("The relay refused to open to %s (%d %s)."), *Host, Item.Code, *Item.Message))));
			}
			else
			{
				AnswerOpeningWaiters(Item.Ip, TFlockRelayResult<bool>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::Refused,
					FString::Printf(TEXT("The relay refused the opening to %s (%d %s)."), *Host, Item.Code, *Item.Message))));
			}
			break;
		}
		case FForTheGame::EKind::Warning:
			Logger->LogWarning(Item.Message);
			break;
		}
	}
}

FFlockRelayPeer FFlockRelayConnection::GetAddress() const
{
	return Core->GetAddress();
}

FFlockRelayPeer FFlockRelayConnection::GetServerEndpoint() const
{
	return Core->GetServerEndpoint();
}

FString FFlockRelayConnection::GetServer() const
{
	return Core->Login.ToString();
}

TArray<FFlockRelayPeer> FFlockRelayConnection::GetStunServers() const
{
	return Core->GetStunServers();
}

bool FFlockRelayConnection::IsOpen() const
{
	return Core->GetState() == EState::Open;
}

bool FFlockRelayConnection::HasStopped() const
{
	const EState State = Core->GetState();
	return State == EState::Closed || State == EState::Failed;
}

FString FFlockRelayConnection::GetFailureReason() const
{
	return Core->GetFailureReason();
}

int64 FFlockRelayConnection::GetPacketsDropped() const
{
	return Core->Packets.GetPacketsDropped();
}

int64 FFlockRelayConnection::GetPacketsSent() const
{
	return Core->Packets.GetPacketsSent();
}

FFlockRequestHandle FFlockRelayConnection::OpenTo(const FFlockRelayPeer& PeerAddress, TFunction<void(TFlockRelayResult<bool>)> OnOpen)
{
	if (!IsOpen())
	{
		OnOpen(TFlockRelayResult<bool>::Fail(NotOpen()));
		return FFlockRequestHandle();
	}
	const uint32 Ip = PeerAddress.Address;
	if (Core->Packets.IsOpenedTo(Ip))
	{
		OnOpen(TFlockRelayResult<bool>::Ok(true));
		return FFlockRequestHandle();
	}
	if (Refused.Contains(Ip))
	{
		OnOpen(TFlockRelayResult<bool>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::OpenRefused,
			FString::Printf(TEXT("The relay refused to open to %s before."), *PeerAddress.Ip()))));
		return FFlockRequestHandle();
	}
	if (!Asked.Contains(Ip))
	{
		const int32 Most = MostOpeningsForTesting > 0 ? MostOpeningsForTesting : MostOpenings;
		if (Core->Packets.GetOpeningsCount() + Asked.Num() >= Most)
		{
			// Remembered as a refusal, so seats naming ever more addresses cannot make the relay renew openings without end.
			Refused.Add(Ip);
			OnOpen(TFlockRelayResult<bool>::Fail(FFlockRelayFailure::Of(FlockRelayFailureReasons::OpenRefused,
				FString::Printf(TEXT("This relay address already lets in %d IPs, the most it does; %s is not let in."), Most, *PeerAddress.Ip()))));
			return FFlockRequestHandle();
		}
		Asked.Add(Ip);
		TArray<FFlockRelayAttribute> Attributes;
		Attributes.Emplace(FFlockRelayWire::XorPeerAddressAttribute, FFlockRelayWire::AddressValue(FFlockRelayPeer(Ip, 0)));
		FFlockRelayCore* CoreOnItsThread = &Core.Get();
		Core->HandOver(MakeShared<FFlockRelayRequest, ESPMode::ThreadSafe>(FFlockRelayWire::CreatePermissionMethod,
			[CoreOnItsThread, Ip](int32 Code, const FString& Words, const FFlockRelayMessage*)
			{
				// Opened before the game hears of it, so a caller in between finds it open rather than asking again.
				if (Code == 0 && CoreOnItsThread->Packets.AddOpening(Ip))
				{
					CoreOnItsThread->Renewals.KeepOpening(Ip, FPlatformTime::Seconds());
				}
				FForTheGame Answered;
				Answered.Kind = FForTheGame::EKind::OpeningAnswered;
				Answered.Ip = Ip;
				Answered.Code = Code;
				Answered.Message = Words;
				CoreOnItsThread->Hand(Answered);
			}, MoveTemp(Attributes)));
	}
	const FOpeningWaiterRef Waiter = MakeShared<FOpeningWaiter>();
	Waiter->Ip = Ip;
	Waiter->OnOpen = MoveTemp(OnOpen);
	Waiters.Add(Waiter);
	const TSharedRef<FFlockCancelToken> Token = MakeShared<FFlockCancelToken>();
	const TWeakPtr<FFlockRelayConnection> WeakSelf = AsShared();
	const TWeakPtr<FOpeningWaiter> WeakWaiter = Waiter;
	// The caller may stop waiting; the request itself goes on, and what it opens is kept.
	Token->Canceller = [WeakSelf, WeakWaiter]()
	{
		const TSharedPtr<FFlockRelayConnection> Self = WeakSelf.Pin();
		const TSharedPtr<FOpeningWaiter> Pinned = WeakWaiter.Pin();
		if (Self.IsValid() && Pinned.IsValid() && Self->Waiters.Remove(Pinned.ToSharedRef()) > 0 && Pinned->OnOpen)
		{
			Pinned->OnOpen(TFlockRelayResult<bool>::Fail(FFlockRelayFailure::Cancelled(TEXT("The wait for the relay's opening was cancelled"))));
		}
	};
	return FFlockRequestHandle(Token);
}

bool FFlockRelayConnection::IsOpenTo(const FFlockRelayPeer& PeerAddress) const
{
	return Core->Packets.IsOpenedTo(PeerAddress.Address);
}

bool FFlockRelayConnection::RefusedToOpenTo(const FFlockRelayPeer& PeerAddress) const
{
	return Refused.Contains(PeerAddress.Address);
}

bool FFlockRelayConnection::Send(const FFlockRelayPeer& To, const uint8* Data, int32 Count)
{
	return IsOpen() && Core->Packets.Send(To, Data, Count);
}

bool FFlockRelayConnection::TryReceive(uint8* Into, int32 IntoSize, int32& OutCount, FFlockRelayPeer& OutFrom)
{
	return Core->Packets.TryReceive(Into, IntoSize, OutCount, OutFrom);
}

void FFlockRelayConnection::KeepChannelTo(const FFlockRelayPeer& Peer)
{
	Core->Packets.WantChannel(Peer);
}

void FFlockRelayConnection::LetChannelGo(const FFlockRelayPeer& Peer)
{
	Core->Packets.LetChannelGo(Peer);
}

bool FFlockRelayConnection::HandArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& Receiver)
{
	return Core->Packets.HandArrivalsTo(Receiver);
}

void FFlockRelayConnection::StopHandingArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& Receiver)
{
	Core->Packets.StopHandingArrivalsTo(Receiver);
}

bool FFlockRelayConnection::HandsArrivalsOver() const
{
	return Core->Packets.HandsArrivalsOver();
}

void FFlockRelayConnection::Close()
{
	const TSharedRef<FFlockRelayConnection> KeepWhileAnswering = AsShared();
	Core->StopConnection(EState::Closed, FString(), FString());
	const FFlockRelayFailure Cancelled = FFlockRelayFailure::Cancelled(TEXT("The relay connection was closed while the call waited"));
	AnswerOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>::Fail(Cancelled));
	Asked.Reset();
	AnswerEveryWaiter(TFlockRelayResult<bool>::Fail(Cancelled));
}

void FFlockRelayConnection::StopEveryRelayThread()
{
	FFlockRelayThreads::Get().StopAll();
}

int32 FFlockRelayConnection::GetRelayThreadsForTesting()
{
	return FFlockRelayThreads::Get().Count();
}

int32 FFlockRelayConnection::GetNoncesTakenForTesting() const
{
	return Core->Requests.GetNoncesTaken();
}

int32 FFlockRelayConnection::GetLocalPortForTesting() const
{
	return Core->Socket.GetLocalPort();
}

FName FFlockRelayConnection::GetSocketProtocolForTesting() const
{
	return Core->Socket.GetProtocolForTesting();
}

bool FFlockRelayConnection::KeepsPacketsForTesting() const
{
	return Core->Packets.KeepsPacketsForTesting();
}

bool FFlockRelayConnection::KeepsChannelToForTesting(const FFlockRelayPeer& Peer) const
{
	return Core->Packets.KeepsChannelTo(Peer);
}

uint16 FFlockRelayConnection::ChannelNumberForTesting(const FFlockRelayPeer& Peer) const
{
	return Core->Packets.ChannelFor(Peer);
}

uint32 FFlockRelayConnection::GetThreadIdForTesting() const
{
	return Core->ThreadId;
}

FFlockRelayFailure FFlockRelayConnection::NotOpen() const
{
	// Only a connection that has stopped is handed back not open: closed by its owner, or failed for a reason.
	const FString Reason = Core->GetFailureReason();
	if (!Reason.IsEmpty())
	{
		return FFlockRelayFailure::Of(Reason, FString::Printf(TEXT("The relay connection stopped (%s)."), *Reason));
	}
	return FFlockRelayFailure::Of(FlockRelayFailureReasons::Closed, TEXT("The relay connection was closed."));
}

void FFlockRelayConnection::AnswerOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Result)
{
	// Answered once: taken out before it runs, so an answer it causes cannot answer it again.
	FOnOpened Answer = MoveTemp(OnOpened);
	OnOpened = nullptr;
	if (Answer)
	{
		Answer(MoveTemp(Result));
	}
}

void FFlockRelayConnection::AnswerOpeningWaiters(uint32 Ip, const TFlockRelayResult<bool>& Result)
{
	TArray<FOpeningWaiterRef> Answering;
	for (int32 Index = Waiters.Num() - 1; Index >= 0; --Index)
	{
		if (Waiters[Index]->Ip == Ip)
		{
			Answering.Insert(Waiters[Index], 0);
			Waiters.RemoveAt(Index);
		}
	}
	for (const FOpeningWaiterRef& Waiter : Answering)
	{
		if (Waiter->OnOpen)
		{
			Waiter->OnOpen(Result);
		}
	}
}

void FFlockRelayConnection::AnswerEveryWaiter(const TFlockRelayResult<bool>& Result)
{
	const TArray<FOpeningWaiterRef> Answering = MoveTemp(Waiters);
	Waiters.Reset();
	for (const FOpeningWaiterRef& Waiter : Answering)
	{
		if (Waiter->OnOpen)
		{
			Waiter->OnOpen(Result);
		}
	}
}
