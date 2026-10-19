// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayBridge.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Multiplayer/Relay/FlockRelayThreads.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include <atomic>

int32 FFlockRelayBridge::MostPlayersForTesting = 0;
double FFlockRelayBridge::NotLetInWithinForTesting = 0.0;
bool FFlockRelayBridge::bSocketRefusedForTesting = false;

/** What a bridge's loopback threads share with it. */
struct FFlockRelayBridgeShared
{
	/** A host's: the port its net driver listens on, which every loopback socket sends to. */
	std::atomic<int32> NetDriverPort{ 0 };
	/** The engine sent a packet the relay cannot carry: told once, on the game thread. */
	std::atomic<bool> bSentTooLarge{ false };
};

namespace
{
	/** How long a loopback socket's thread waits on it at a time, so an end is seen within this. */
	constexpr double LongestWaitSeconds = 0.05;
	/** The most packets read in one turn, so an end is seen even while the engine sends without a pause. */
	constexpr int32 MostReadsATurn = 256;
}

/**
 * One loopback socket standing in for a relayed peer: what the engine sends to it goes to the peer through the relay, and what
 * the peer sends reaches the engine from it. Its own thread reads it; the relay's thread sends from it. The socket is closed by
 * its thread when it ends (or when it is gone, if the thread never started), never while the relay's thread sends from it.
 */
class FFlockRelayLoopback : public IFlockRelayThreadWork
{
public:
	FFlockRelayLoopback(FSocket* InSocket, const FFlockRelayPeer& InPeer, const TSharedRef<FFlockRelayConnection>& InRelay,
		const TSharedRef<FFlockRelayBridgeShared, ESPMode::ThreadSafe>& InShared, bool bInFollowsTheEngine)
		: Port(FFlockUdpSockets::LocalPortOf(*InSocket))
		, Peer(InPeer)
		, Relay(InRelay)
		, Shared(InShared)
		, bFollowsTheEngine(bInFollowsTheEngine)
		, ToTheEngine(FFlockUdpSockets::OnLoopback(0))
		, From(FFlockUdpSockets::OnLoopback(0))
		, Socket(InSocket)
	{
	}

	virtual ~FFlockRelayLoopback() override
	{
		CloseSocket();
	}

	int32 GetPort() const { return Port; }

	/** Stops forwarding at once; its thread closes the socket within one wait. Any thread. */
	void End() { bEnding = true; }

	virtual bool HasEnded() const override { return bEnded.load(); }

	virtual void EndNow() override { bEnding = true; }

	/**
	 * The relay's thread: hands a packet from the peer to the engine. A player's socket that has not heard its game yet has
	 * nowhere to send it, so it is dropped; the engine's own resends cover a lost packet.
	 */
	void HandToTheEngine(const uint8* Data, int32 Count)
	{
		const int32 EnginePort = bFollowsTheEngine ? GamesPort.load() : Shared->NetDriverPort.load();
		if (EnginePort <= 0)
		{
			return;
		}
		FScopeLock Locked(&SocketLock);
		if (Socket == nullptr || bEnding)
		{
			return;
		}
		ToTheEngine->SetPort(EnginePort);
		int32 Sent = 0;
		Socket->SendTo(Data, Count, Sent, *ToTheEngine);
	}

	// ---- the loopback socket's own thread ----

	virtual uint32 Run() override
	{
		while (!bEnding)
		{
			if (Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(LongestWaitSeconds)))
			{
				ReadEverythingWaiting();
			}
		}
		CloseSocket();
		bEnded = true;
		return 0;
	}

private:
	/** What the engine sent, on through the relay to the peer this socket stands for. */
	void ReadEverythingWaiting()
	{
		ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		for (int32 Reads = 0; Reads < MostReadsATurn && !bEnding; ++Reads)
		{
			int32 Read = 0;
			if (!Socket->RecvFrom(Received, UE_ARRAY_COUNT(Received), Read, *From))
			{
				const ESocketErrors Error = Sockets != nullptr ? Sockets->GetLastErrorCode() : SE_NO_ERROR;
				if (Error == SE_EMSGSIZE)
				{
					Shared->bSentTooLarge = true;
					continue;
				}
				// A send of the relay's found the engine's port closed for a moment (port unreachable): it says nothing of what
				// comes next. Anything else is nothing more waiting.
				if (Error == SE_ECONNRESET || Error == SE_UDP_ERR_PORT_UNREACH)
				{
					continue;
				}
				return;
			}
			// The socket is bound to 127.0.0.1, so only this device sends to it: the host's net driver on its port, or the game this
			// socket stands in for the host to.
			if (Read <= 0)
			{
				continue;
			}
			if (bFollowsTheEngine)
			{
				// The net driver sends from a new port each time it connects (measured), so the port it last sent from is the one.
				GamesPort.store(From->GetPort());
			}
			else if (From->GetPort() != Shared->NetDriverPort.load())
			{
				continue;
			}
			if (Read > FFlockRelayConnection::MostBytesInAPacket)
			{
				Shared->bSentTooLarge = true;
				continue;
			}
			Relay->Send(Peer, Received, Read);
		}
	}

	void CloseSocket()
	{
		FScopeLock Locked(&SocketLock);
		if (Socket != nullptr)
		{
			FFlockUdpSockets::Close(Socket);
			Socket = nullptr;
		}
	}

	const int32 Port;
	const FFlockRelayPeer Peer;
	const TSharedRef<FFlockRelayConnection> Relay;
	const TSharedRef<FFlockRelayBridgeShared, ESPMode::ThreadSafe> Shared;
	/** A player's: it sends to the port its game last sent from. A host's sends to the net driver's port. */
	const bool bFollowsTheEngine;
	/** The relay's thread's own: where a packet for the engine goes. */
	const TSharedRef<FInternetAddr> ToTheEngine;
	/** The loopback thread's own: where a packet it read came from. */
	const TSharedRef<FInternetAddr> From;
	/** Guards the socket against being closed while the relay's thread sends from it. */
	FCriticalSection SocketLock;
	FSocket* Socket = nullptr;
	std::atomic<int32> GamesPort{ 0 };
	std::atomic<bool> bEnding{ false };
	std::atomic<bool> bEnded{ false };
	/** One more byte than the relay carries, so a packet too large is seen whole or as too large, never cut to fit. */
	uint8 Received[FFlockRelayConnection::MostBytesInAPacket + 1];
};

FFlockRelayBridge::FFlockRelayBridge(const TSharedRef<FFlockRelayConnection>& InRelay, const TSharedRef<IFlockLogger>& InLogger,
	bool bInForTheHost, const FFlockRelayPeer& InHost)
	: Relay(InRelay)
	, Logger(InLogger)
	, bForTheHost(bInForTheHost)
	, Host(InHost)
	, Shared(MakeShared<FFlockRelayBridgeShared, ESPMode::ThreadSafe>())
{
}

FFlockRelayBridge::~FFlockRelayBridge()
{
	// Its owner normally closed it. One dropped without closing lets go here: the relay holds it weakly, so its arrivals go back
	// to the relay by themselves, and the forwarding and channels left stop. Any thread: the relay's may hold the last reference.
	if (bClosed)
	{
		return;
	}
	if (HostLoopback.IsValid())
	{
		HostLoopback->End();
		Relay->LetChannelGo(Host);
	}
	for (const TPair<FFlockRelayPeer, FPeer>& Entry : Peers)
	{
		Entry.Value.Loopback->End();
		Relay->LetChannelGo(Entry.Key);
	}
}

TFlockRelayResult<FFlockRelayBridge::FPtr> FFlockRelayBridge::ToTheHost(const TSharedRef<FFlockRelayConnection>& Relay, const FFlockRelayPeer& Host,
	const TSharedRef<IFlockLogger>& Logger)
{
	const FRef Bridge = FRef(new FFlockRelayBridge(Relay, Logger, /*bForTheHost*/ false, Host));
	FFlockRelayFailure Failure;
	Bridge->HostLoopback = Bridge->NewLoopback(Host, Failure);
	if (!Bridge->HostLoopback.IsValid())
	{
		return TFlockRelayResult<FPtr>::Fail(Failure);
	}
	return Register(Bridge);
}

TFlockRelayResult<FFlockRelayBridge::FPtr> FFlockRelayBridge::ForTheHost(const TSharedRef<FFlockRelayConnection>& Relay, int32 NetDriverPort,
	const TSharedRef<IFlockLogger>& Logger)
{
	if (NetDriverPort < 1 || NetDriverPort > 65535)
	{
		FFlockRelayFailure Failure;
		Failure.Error = FFlockError::MakeWithFix(EFlockErrorType::Validation, FString::Printf(TEXT("The net driver's port must be from 1 to 65535, not %d"),
			NetDriverPort), TEXT("Give the port the host's level listens on."));
		return TFlockRelayResult<FPtr>::Fail(Failure);
	}
	const FRef Bridge = FRef(new FFlockRelayBridge(Relay, Logger, /*bForTheHost*/ true, FFlockRelayPeer()));
	Bridge->Shared->NetDriverPort = NetDriverPort;
	return Register(Bridge);
}

TFlockRelayResult<FFlockRelayBridge::FPtr> FFlockRelayBridge::Register(const FRef& Bridge)
{
	if (!Bridge->Relay->HandArrivalsTo(Bridge))
	{
		// Another receiver's channels stay as they are: only this bridge's own socket is let go.
		{
			FScopeLock Locked(&Bridge->Gate);
			Bridge->bClosed = true;
		}
		if (Bridge->HostLoopback.IsValid())
		{
			Bridge->HostLoopback->End();
			Bridge->HostLoopback.Reset();
		}
		FFlockRelayFailure Failure;
		Failure.Error = FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("The session's relay already hands its packets to another receiver on this device"),
			TEXT("Leave the host this game is connected to through the relay, or close what reads the relay's packets, before traveling again."));
		return TFlockRelayResult<FPtr>::Fail(Failure);
	}
	// The host is the one peer a player's game sends to, so it gets a channel at once; a host's peers get one when let in.
	if (!Bridge->bForTheHost)
	{
		Bridge->Relay->KeepChannelTo(Bridge->Host);
	}
	return TFlockRelayResult<FPtr>::Ok(Bridge);
}

TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> FFlockRelayBridge::NewLoopback(const FFlockRelayPeer& Peer, FFlockRelayFailure& OutFailure) const
{
	FSocket* Socket = bSocketRefusedForTesting ? nullptr : FFlockUdpSockets::OpenOnLoopback(ThreadName);
	if (Socket == nullptr)
	{
		OutFailure.Error = FFlockError::MakeWithFix(EFlockErrorType::Network,
			TEXT("No UDP socket could be opened on this device's loopback (127.0.0.1) to hand the relay's packets to the engine"),
			TEXT("Check that a firewall or security tool lets the game open UDP sockets on 127.0.0.1, then travel again; otherwise players connect directly."));
		return nullptr;
	}
	const TSharedRef<FFlockRelayLoopback, ESPMode::ThreadSafe> Loopback = MakeShared<FFlockRelayLoopback, ESPMode::ThreadSafe>(Socket, Peer, Relay,
		Shared, /*bFollowsTheEngine*/ !bForTheHost);
	if (!FFlockRelayThreads::Start(Loopback, ThreadName, ThreadStackBytes))
	{
		OutFailure = FFlockRelayFailure::Of(FlockRelayFailureReasons::NoThread,
			TEXT("No thread could be started to hand the relay's packets to the engine: this launch of the game runs without threads"));
		return nullptr;
	}
	return Loopback;
}

int32 FFlockRelayBridge::GetLocalPort() const
{
	FScopeLock Locked(&Gate);
	return HostLoopback.IsValid() ? HostLoopback->GetPort() : 0;
}

bool FFlockRelayBridge::IsClosed() const
{
	FScopeLock Locked(&Gate);
	return bClosed;
}

void FFlockRelayBridge::SetNetDriverPort(int32 Port)
{
	if (Port > 0 && Port <= 65535)
	{
		Shared->NetDriverPort = Port;
	}
}

bool FFlockRelayBridge::TryGetPeer(int32 LoopbackPort, FFlockRelayPeer& OutPeer) const
{
	FScopeLock Locked(&Gate);
	for (const TPair<FFlockRelayPeer, FPeer>& Entry : Peers)
	{
		if (Entry.Value.Loopback->GetPort() == LoopbackPort)
		{
			OutPeer = Entry.Key;
			return true;
		}
	}
	return false;
}

int32 FFlockRelayBridge::LoopbackPortOf(const FFlockRelayPeer& Peer) const
{
	FScopeLock Locked(&Gate);
	const FPeer* Found = Peers.Find(Peer);
	return Found != nullptr ? Found->Loopback->GetPort() : 0;
}

void FFlockRelayBridge::LetIn(const FFlockRelayPeer& Peer)
{
	{
		FScopeLock Locked(&Gate);
		FPeer* Found = Peers.Find(Peer);
		if (bClosed || Found == nullptr)
		{
			return;
		}
		Found->bWasLetIn = true;
	}
	Relay->KeepChannelTo(Peer);
}

void FFlockRelayBridge::Forget(const FFlockRelayPeer& Peer)
{
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Loopback;
	{
		FScopeLock Locked(&Gate);
		FPeer Removed;
		if (!Peers.RemoveAndCopyValue(Peer, Removed))
		{
			return;
		}
		Loopback = Removed.Loopback;
	}
	Loopback->End();
	Relay->LetChannelGo(Peer);
}

void FFlockRelayBridge::NoLongerLetIn(const FFlockRelayPeer& Peer)
{
	{
		FScopeLock Locked(&Gate);
		FPeer* Found = Peers.Find(Peer);
		if (Found == nullptr)
		{
			return;
		}
		Found->bWasLetIn = false;
		Found->WaitingSince = FPlatformTime::Seconds();
	}
	Relay->LetChannelGo(Peer);
}

void FFlockRelayBridge::Tick()
{
	if (Shared->bSentTooLarge && !bToldTooLarge)
	{
		bToldTooLarge = true;
		Logger->LogWarning(FString::Printf(TEXT("The engine sent a packet through Flock's relay larger than the %d bytes it carries; such packets are "
			"dropped. Keep the net driver's largest packet at the engine's own (1024 bytes)."), FFlockRelayConnection::MostBytesInAPacket));
	}
}

void FFlockRelayBridge::Close()
{
	TArray<TPair<FFlockRelayPeer, FPeer>> Closing;
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Standing;
	{
		FScopeLock Locked(&Gate);
		if (bClosed)
		{
			return;
		}
		bClosed = true;
		Closing = Peers.Array();
		Peers.Reset();
		Standing = MoveTemp(HostLoopback);
		HostLoopback.Reset();
	}
	Relay->StopHandingArrivalsTo(AsShared());
	if (Standing.IsValid())
	{
		Standing->End();
		Relay->LetChannelGo(Host);
	}
	for (const TPair<FFlockRelayPeer, FPeer>& Entry : Closing)
	{
		Entry.Value.Loopback->End();
		Relay->LetChannelGo(Entry.Key);
	}
}

void FFlockRelayBridge::Arrived(const FFlockRelayPeer& From, const uint8* Data, int32 Count)
{
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Loopback;
	{
		FScopeLock Locked(&Gate);
		if (bClosed)
		{
			return;
		}
		if (!bForTheHost)
		{
			// A player's game hears its host alone: anyone else the relay lets in is dropped.
			if (From != Host)
			{
				return;
			}
			Loopback = HostLoopback;
		}
		else if (FPeer* Found = Peers.Find(From))
		{
			Loopback = Found->Loopback;
		}
		else
		{
			const double Now = FPlatformTime::Seconds();
			ForgetPeersNeverLetIn(Now);
			if (Peers.Num() >= (MostPlayersForTesting > 0 ? MostPlayersForTesting : MostPlayers))
			{
				return;
			}
			// No socket or thread now: the peer's own resends try again.
			FFlockRelayFailure Failure;
			Loopback = NewLoopback(From, Failure);
			if (!Loopback.IsValid())
			{
				return;
			}
			FPeer& Added = Peers.Add(From);
			Added.Loopback = Loopback;
			Added.WaitingSince = Now;
		}
	}
	if (Loopback.IsValid())
	{
		Loopback->HandToTheEngine(Data, Count);
	}
}

void FFlockRelayBridge::ForgetPeersNeverLetIn(double Now)
{
	const double Given = NotLetInWithinForTesting > 0.0 ? NotLetInWithinForTesting : NotLetInWithinSeconds;
	for (auto It = Peers.CreateIterator(); It; ++It)
	{
		if (!It.Value().bWasLetIn && Now - It.Value().WaitingSince >= Given)
		{
			It.Value().Loopback->End();
			It.RemoveCurrent();
		}
	}
}

int32 FFlockRelayBridge::GetPeersForTesting() const
{
	FScopeLock Locked(&Gate);
	return Peers.Num();
}

TArray<int32> FFlockRelayBridge::GetPeerPortsForTesting() const
{
	FScopeLock Locked(&Gate);
	TArray<int32> Ports;
	for (const TPair<FFlockRelayPeer, FPeer>& Entry : Peers)
	{
		Ports.Add(Entry.Value.Loopback->GetPort());
	}
	return Ports;
}

int32 FFlockRelayBridge::GetNetDriverPortForTesting() const
{
	return Shared->NetDriverPort.load();
}
