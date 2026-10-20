// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Multiplayer/Relay/FlockRelayBridge.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"
#include "Multiplayer/FlockUdpSockets.h"
#include "Multiplayer/Relay/FlockBridgeMessages.h"
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
	/** A player's: the host's bridge reads the bridges' messages, so each new connection of the game is told to it. Set before any thread starts. */
	bool bTellsNewConnections = false;
	/** A player's: the number the first connection of its game is told by; random, so a new bridge's first is told apart from an earlier bridge's last. */
	uint32 FirstConnectionNumber = 0;
	/** A player's: the connections its game started through the bridge, counted by the loopback's thread. */
	std::atomic<int32> ConnectionsStarted{ 0 };
	/** A player's: Travel To Session saw its game start connecting to the host again, so the next packet begins a new connection. */
	std::atomic<bool> bTheGameConnectsAgain{ false };
};

namespace
{
	/** How long a loopback socket's thread waits on it at a time, so an end is seen within this. */
	constexpr double LongestWaitSeconds = 0.05;
	/** The most packets read in one turn, so an end is seen even while the engine sends without a pause. */
	constexpr int32 MostReadsATurn = 256;
}

/**
 * One loopback socket standing in for a relayed peer: what the engine sends to it goes to the peer through the relay (or on the
 * peer's direct link, while that is direct), and what the peer sends reaches the engine from it. Its own thread reads it; the
 * relay's thread and the peer's link send from it. The socket is closed by its thread when it ends (or when it is gone, if the
 * thread never started), never while another thread sends from it.
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

	/** The peer's direct link the engine's packets try first, or null for the relay alone. Any thread. */
	void SetLink(const FFlockDirectLink::FPtr& InLink)
	{
		FScopeLock Locked(&LinkLock);
		Link = InLink;
	}

	/**
	 * The relay's thread, or the peer's link's: hands a packet from the peer to the engine. A player's socket that has not heard its
	 * game yet has nowhere to send it, so it is dropped; the engine's own resends cover a lost packet.
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
	/** What the engine sent, on to the peer this socket stands for: directly while its link is direct, else through the relay. */
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
				const int32 Before = GamesPort.load();
				GamesPort.store(From->GetPort());
				// A new connection of the game's: the bridge's first, or one Travel To Session saw the engine start. A new port alone may
				// be a socket the engine remade for the same connection (net.RecreateSocketTimeoutThreshold), which keeps its place.
				const bool bFirst = Before == 0;
				if (bFirst || Shared->bTheGameConnectsAgain.exchange(false))
				{
					// A later connection's packets follow word of it through the relay, so the host has moved it to its new place by the
					// time they arrive; its own link comes with the bridge's next frame. The first keeps the link made with the bridge.
					if (!bFirst)
					{
						SetLink(nullptr);
					}
					const int32 Started = ++Shared->ConnectionsStarted;
					NewConnectionNumber = Shared->FirstConnectionNumber + static_cast<uint32>(Started - 1);
					NewConnectionTellsLeft = Shared->bTellsNewConnections ? FFlockRelayBridge::NewConnectionToldTimes : 0;
				}
				if (NewConnectionTellsLeft > 0)
				{
					--NewConnectionTellsLeft;
					const TArray<uint8> Word = FFlockBridgeMessages::WriteNewConnection(NewConnectionNumber);
					Relay->Send(Peer, Word.GetData(), Word.Num());
				}
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
			FFlockDirectLink::FPtr Direct;
			{
				FScopeLock Locked(&LinkLock);
				Direct = Link;
			}
			if (!Direct.IsValid() || !Direct->TrySend(Received, Read))
			{
				Relay->Send(Peer, Received, Read);
			}
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
	/** Where a packet for the engine goes; used under SocketLock, by the relay's thread and the peer's link alike. */
	const TSharedRef<FInternetAddr> ToTheEngine;
	/** The loopback thread's own: where a packet it read came from. */
	const TSharedRef<FInternetAddr> From;
	/** Guards the socket against being closed while another thread sends from it. */
	FCriticalSection SocketLock;
	FSocket* Socket = nullptr;
	FCriticalSection LinkLock;
	FFlockDirectLink::FPtr Link;
	std::atomic<int32> GamesPort{ 0 };
	/** The loopback thread's own: the number its game's newest connection is told by, and how many more of its packets go behind word of it. */
	uint32 NewConnectionNumber = 0;
	int32 NewConnectionTellsLeft = 0;
	std::atomic<bool> bEnding{ false };
	std::atomic<bool> bEnded{ false };
	/** One more byte than the relay carries, so a packet too large is seen whole or as too large, never cut to fit. */
	uint8 Received[FFlockRelayConnection::MostBytesInAPacket + 1];
};

FFlockRelayBridge::FFlockRelayBridge(const TSharedRef<FFlockRelayConnection>& InRelay, const TSharedRef<IFlockLogger>& InLogger,
	bool bInForTheHost, const FFlockRelayPeer& InHost, bool bInGoesDirect)
	: Relay(InRelay)
	, Logger(InLogger)
	, bForTheHost(bInForTheHost)
	, Host(InHost)
	, Shared(MakeShared<FFlockRelayBridgeShared, ESPMode::ThreadSafe>())
	, bGoesDirect(bInGoesDirect)
{
}

FFlockRelayBridge::~FFlockRelayBridge()
{
	// Its owner normally closed it. One dropped without closing lets go here: the relay holds it weakly, so its arrivals go back
	// to the relay by themselves, and the forwarding, links and channels left stop. Any thread: the relay's or a link's may hold
	// the last reference.
	if (bClosed)
	{
		return;
	}
	if (HostLink.IsValid())
	{
		HostLink->Close();
	}
	if (HostLoopback.IsValid())
	{
		HostLoopback->End();
		Relay->LetChannelGo(Host);
	}
	for (const TPair<FFlockRelayPeer, FPeer>& Entry : Peers)
	{
		if (Entry.Value.Link.IsValid())
		{
			Entry.Value.Link->Close();
		}
		Entry.Value.Loopback->End();
		Relay->LetChannelGo(Entry.Key);
	}
}

TFlockRelayResult<FFlockRelayBridge::FPtr> FFlockRelayBridge::ToTheHost(const TSharedRef<FFlockRelayConnection>& Relay, const FFlockRelayPeer& Host,
	const TSharedRef<IFlockLogger>& Logger, const FToTheHostOptions& Options)
{
	// A direct path is opened with messages only a bridge that reads them may hear.
	const FRef Bridge = FRef(new FFlockRelayBridge(Relay, Logger, /*bForTheHost*/ false, Host, Options.bGoesDirect && Options.bHostReadsBridgeMessages));
	Bridge->Shared->bTellsNewConnections = Options.bHostReadsBridgeMessages;
	const FFlockBridgeKey Random = FFlockBridgeKey::NewRandom();
	Bridge->Shared->FirstConnectionNumber = (uint32(Random.Bytes[0]) << 24) | (uint32(Random.Bytes[1]) << 16) | (uint32(Random.Bytes[2]) << 8) | uint32(Random.Bytes[3]);
	FFlockRelayFailure Failure;
	Bridge->HostLoopback = Bridge->NewLoopback(Host, Failure);
	if (!Bridge->HostLoopback.IsValid())
	{
		return TFlockRelayResult<FPtr>::Fail(Failure);
	}
	const TFlockRelayResult<FPtr> Registered = Register(Bridge);
	if (Registered.bSuccess && Bridge->bGoesDirect)
	{
		// The link for the game's first connection; each later one gets its own (Tick).
		const FFlockDirectLink::FPtr Link = Bridge->NewLink(Host);
		FScopeLock Locked(&Bridge->Gate);
		Bridge->HostLink = Link;
		Bridge->ConnectionsLinked = 1;
		Bridge->HostLoopback->SetLink(Link);
	}
	return Registered;
}

TFlockRelayResult<FFlockRelayBridge::FPtr> FFlockRelayBridge::ForTheHost(const TSharedRef<FFlockRelayConnection>& Relay, int32 NetDriverPort,
	const TSharedRef<IFlockLogger>& Logger, bool bGoesDirect)
{
	if (NetDriverPort < 1 || NetDriverPort > 65535)
	{
		FFlockRelayFailure Failure;
		Failure.Error = FFlockError::MakeWithFix(EFlockErrorType::Validation, FString::Printf(TEXT("The net driver's port must be from 1 to 65535, not %d"),
			NetDriverPort), TEXT("Give the port the host's level listens on."));
		return TFlockRelayResult<FPtr>::Fail(Failure);
	}
	const FRef Bridge = FRef(new FFlockRelayBridge(Relay, Logger, /*bForTheHost*/ true, FFlockRelayPeer(), bGoesDirect));
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

FFlockDirectLink::FPtr FFlockRelayBridge::NewLink(const FFlockRelayPeer& Peer) const
{
	const TSharedRef<FFlockRelayConnection> Through = Relay;
	const TWeakPtr<const FFlockRelayBridge, ESPMode::ThreadSafe> WeakSelf = AsShared();
	return FFlockDirectLink::Start(Relay->GetServerEndpoint(), Relay->GetStunServers(),
		[Through, Peer](const TArray<uint8>& Message)
		{
			Through->Send(Peer, Message.GetData(), Message.Num());
		},
		[WeakSelf, Peer](const uint8* Data, int32 Count)
		{
			if (const TSharedPtr<const FFlockRelayBridge, ESPMode::ThreadSafe> Self = WeakSelf.Pin())
			{
				Self->HandToTheEngineFrom(Peer, Data, Count);
			}
		},
		bForTheHost ? FString::Printf(TEXT("the player on relay address %s"), *Peer.ToString()) : FString(TEXT("the host")));
}

bool FFlockRelayBridge::OffersADirectPathTo(const FFlockRelayPeer& Peer, const FPeer& Entry) const
{
	return bForTheHost && bGoesDirect && Entry.bWasLetIn && PeersReadingBridgeMessages.Contains(Peer);
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

void FFlockRelayBridge::GameConnectsAgain()
{
	if (!bForTheHost)
	{
		Shared->bTheGameConnectsAgain = true;
	}
}

bool FFlockRelayBridge::IsToldTheGameConnectsAgainForTesting() const
{
	return Shared->bTheGameConnectsAgain.load();
}

void FFlockRelayBridge::SetNetDriverPort(int32 Port)
{
	if (Port > 0 && Port <= 65535)
	{
		Shared->NetDriverPort = Port;
	}
}

void FFlockRelayBridge::SetGoesDirect(bool bInGoesDirect)
{
	TArray<FFlockDirectLink::FPtr> Closing;
	TArray<FFlockRelayPeer> Offered;
	{
		FScopeLock Locked(&Gate);
		if (bClosed || !bForTheHost || bGoesDirect == bInGoesDirect)
		{
			return;
		}
		bGoesDirect = bInGoesDirect;
		for (TPair<FFlockRelayPeer, FPeer>& Entry : Peers)
		{
			if (!bGoesDirect && Entry.Value.Link.IsValid())
			{
				Closing.Add(MoveTemp(Entry.Value.Link));
				Entry.Value.Link.Reset();
				Entry.Value.Loopback->SetLink(nullptr);
			}
			else if (OffersADirectPathTo(Entry.Key, Entry.Value))
			{
				Offered.Add(Entry.Key);
			}
		}
	}
	for (const FFlockDirectLink::FPtr& Link : Closing)
	{
		Link->Close();
	}
	for (const FFlockRelayPeer& Peer : Offered)
	{
		GiveALink(Peer);
	}
}

void FFlockRelayBridge::SetPeersThatReadBridgeMessages(const TSet<FFlockRelayPeer>& InPeers)
{
	TArray<FFlockRelayPeer> Offered;
	{
		FScopeLock Locked(&Gate);
		if (bClosed || !bForTheHost)
		{
			return;
		}
		PeersReadingBridgeMessages = InPeers;
		for (const TPair<FFlockRelayPeer, FPeer>& Entry : Peers)
		{
			if (!Entry.Value.Link.IsValid() && OffersADirectPathTo(Entry.Key, Entry.Value))
			{
				Offered.Add(Entry.Key);
			}
		}
	}
	for (const FFlockRelayPeer& Peer : Offered)
	{
		GiveALink(Peer);
	}
}

void FFlockRelayBridge::GiveALink(const FFlockRelayPeer& Peer)
{
	// Started outside the lock (a socket and a thread), then kept only if the peer still has its place and no link meanwhile.
	FFlockDirectLink::FPtr Link = NewLink(Peer);
	if (!Link.IsValid())
	{
		return;
	}
	{
		FScopeLock Locked(&Gate);
		FPeer* Found = Peers.Find(Peer);
		if (!bClosed && Found != nullptr && !Found->Link.IsValid() && OffersADirectPathTo(Peer, *Found))
		{
			Found->Link = Link;
			Found->Loopback->SetLink(Link);
			return;
		}
	}
	Link->Close();
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
	bool bOffersADirectPath = false;
	{
		FScopeLock Locked(&Gate);
		FPeer* Found = Peers.Find(Peer);
		if (bClosed || Found == nullptr)
		{
			return;
		}
		Found->bWasLetIn = true;
		bOffersADirectPath = !Found->Link.IsValid() && OffersADirectPathTo(Peer, *Found);
	}
	Relay->KeepChannelTo(Peer);
	if (bOffersADirectPath)
	{
		GiveALink(Peer);
	}
}

void FFlockRelayBridge::Forget(const FFlockRelayPeer& Peer)
{
	FPeer Removed;
	{
		FScopeLock Locked(&Gate);
		if (!Peers.RemoveAndCopyValue(Peer, Removed))
		{
			return;
		}
		if (Removed.Link.IsValid())
		{
			LinksLetGo.Add(Removed.Link);
		}
	}
	if (Removed.Link.IsValid())
	{
		Removed.Link->Close();
	}
	Removed.Loopback->End();
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
	// A player's: each new connection of its game gets a link of its own, since the host's let it in afresh.
	if (!bForTheHost)
	{
		const int32 Started = Shared->ConnectionsStarted.load();
		bool bNewLink = false;
		{
			FScopeLock Locked(&Gate);
			bNewLink = !bClosed && bGoesDirect && Started > ConnectionsLinked;
			if (bNewLink)
			{
				ConnectionsLinked = Started;
			}
		}
		if (bNewLink)
		{
			const FFlockDirectLink::FPtr Made = NewLink(Host);
			FFlockDirectLink::FPtr Replaced;
			{
				FScopeLock Locked(&Gate);
				if (bClosed)
				{
					Replaced = Made;
				}
				else
				{
					Replaced = HostLink;
					HostLink = Made;
					HostLoopback->SetLink(Made);
				}
				if (Replaced.IsValid())
				{
					LinksLetGo.Add(Replaced);
				}
			}
			if (Replaced.IsValid())
			{
				Replaced->Close();
			}
		}
	}
	// The links with something to say: a move to or from a direct path comes a few times a session, so most frames hold none.
	Saying.Reset();
	{
		FScopeLock Locked(&Gate);
		Saying.Append(LinksLetGo);
		LinksLetGo.Reset();
		if (HostLink.IsValid() && HostLink->HasLines())
		{
			Saying.Add(HostLink);
		}
		for (const TPair<FFlockRelayPeer, FPeer>& Entry : Peers)
		{
			if (Entry.Value.Link.IsValid() && Entry.Value.Link->HasLines())
			{
				Saying.Add(Entry.Value.Link);
			}
		}
	}
	for (const FFlockDirectLink::FPtr& Link : Saying)
	{
		for (const FString& Line : Link->TakeLines())
		{
			Logger->LogInfo(Line);
		}
	}
	Saying.Reset();
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
	FFlockDirectLink::FPtr StandingLink;
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
		StandingLink = MoveTemp(HostLink);
		HostLink.Reset();
	}
	Relay->StopHandingArrivalsTo(AsShared());
	if (StandingLink.IsValid())
	{
		StandingLink->Close();
	}
	if (Standing.IsValid())
	{
		Standing->End();
		Relay->LetChannelGo(Host);
	}
	for (const TPair<FFlockRelayPeer, FPeer>& Entry : Closing)
	{
		if (Entry.Value.Link.IsValid())
		{
			Entry.Value.Link->Close();
		}
		Entry.Value.Loopback->End();
		Relay->LetChannelGo(Entry.Key);
	}
}

void FFlockRelayBridge::Arrived(const FFlockRelayPeer& From, const uint8* Data, int32 Count)
{
	// The engine closes a connection any of the bridges' messages reaches (measured), whatever their version.
	if (FFlockBridgeMessages::IsBridgeMessage(Data, Count))
	{
		HeardFromTheOtherBridge(From, Data, Count);
		return;
	}
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Loopback;
	TArray<FFlockDirectLink::FPtr> LinksClosing;
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
			ForgetPeersNeverLetIn(Now, LinksClosing);
			if (Peers.Num() < (MostPlayersForTesting > 0 ? MostPlayersForTesting : MostPlayers))
			{
				// No socket or thread now: the peer's own resends try again.
				FFlockRelayFailure Failure;
				Loopback = NewLoopback(From, Failure);
				if (Loopback.IsValid())
				{
					FPeer& Added = Peers.Add(From);
					Added.Loopback = Loopback;
					Added.WaitingSince = Now;
					TPair<uint32, double> Told;
					if (WordsBeforeTheirPlace.RemoveAndCopyValue(From, Told))
					{
						Added.Connection = Told.Key;
					}
				}
			}
		}
	}
	for (const FFlockDirectLink::FPtr& Link : LinksClosing)
	{
		Link->Close();
	}
	if (Loopback.IsValid())
	{
		Loopback->HandToTheEngine(Data, Count);
	}
}

void FFlockRelayBridge::ForgetPeersNeverLetIn(double Now, TArray<FFlockDirectLink::FPtr>& OutLinksClosing)
{
	const double Given = NotLetInWithinForTesting > 0.0 ? NotLetInWithinForTesting : NotLetInWithinSeconds;
	for (auto It = Peers.CreateIterator(); It; ++It)
	{
		if (!It.Value().bWasLetIn && Now - It.Value().WaitingSince >= Given)
		{
			if (It.Value().Link.IsValid())
			{
				OutLinksClosing.Add(It.Value().Link);
				LinksLetGo.Add(It.Value().Link);
			}
			It.Value().Loopback->End();
			It.RemoveCurrent();
		}
	}
}

void FFlockRelayBridge::HeardFromTheOtherBridge(const FFlockRelayPeer& From, const uint8* Data, int32 Count)
{
	const uint8 Kind = FFlockBridgeMessages::KindOf(Data, Count);
	uint32 Number = 0;
	if (Kind == FFlockBridgeMessages::NewConnection && FFlockBridgeMessages::TryReadNewConnection(Data, Count, Number))
	{
		HeardOfANewConnection(From, Number);
		return;
	}
	FFlockBridgeKey TheirKey;
	TOptional<FFlockBridgeKey> KeyTheyHold;
	TArray<FFlockRelayPeer> TheirAddresses;
	const bool bOffer = Kind == FFlockBridgeMessages::Offer && FFlockBridgeMessages::TryReadOffer(Data, Count, TheirKey, KeyTheyHold, TheirAddresses);
	if (!bOffer && Kind != FFlockBridgeMessages::BackToTheRelay)
	{
		return;
	}
	FFlockDirectLink::FPtr Link;
	bool bANewLinkBehindIt = false;
	{
		FScopeLock Locked(&Gate);
		if (bClosed)
		{
			return;
		}
		if (!bForTheHost)
		{
			Link = From == Host ? HostLink : nullptr;
		}
		else if (const FPeer* Found = Peers.Find(From))
		{
			// A host has links only to players its check let in, so nobody else's is taken. Asked here as well as in ReplaceLink, so an
			// offer from a player not let in again opens no socket or thread for nothing.
			Link = Found->Link;
			bANewLinkBehindIt = bOffer && Link.IsValid() && OffersADirectPathTo(From, *Found) && Link->HeardAnotherKeyThan(TheirKey);
		}
	}
	if (bANewLinkBehindIt)
	{
		// The player's game connected again, so its bridge made a new link: this end makes one for it too.
		Link = ReplaceLink(From, Link);
	}
	if (!Link.IsValid())
	{
		return;
	}
	if (bOffer)
	{
		Link->HeardOffer(TheirKey, KeyTheyHold, TheirAddresses);
	}
	else
	{
		Link->HeardBackToTheRelay(Data, Count);
	}
}

FFlockDirectLink::FPtr FFlockRelayBridge::ReplaceLink(const FFlockRelayPeer& Peer, const FFlockDirectLink::FPtr& Old)
{
	FFlockDirectLink::FPtr Made = NewLink(Peer);
	if (!Made.IsValid())
	{
		// No socket or thread now: the peer stays on the relay, and its next offer asks again.
		return nullptr;
	}
	{
		FScopeLock Locked(&Gate);
		FPeer* Found = Peers.Find(Peer);
		if (!bClosed && Found != nullptr && Found->Link == Old && OffersADirectPathTo(Peer, *Found))
		{
			Found->Link = Made;
			Found->Loopback->SetLink(Made);
			if (Old.IsValid())
			{
				LinksLetGo.Add(Old);
			}
		}
		else
		{
			// Replaced, forgotten or no longer offered meanwhile.
			Made->Close();
			return nullptr;
		}
	}
	if (Old.IsValid())
	{
		Old->Close();
	}
	return Made;
}

void FFlockRelayBridge::HeardOfANewConnection(const FFlockRelayPeer& Peer, uint32 Number)
{
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Ending;
	FFlockDirectLink::FPtr LinkClosing;
	{
		FScopeLock Locked(&Gate);
		if (!bForTheHost || bClosed)
		{
			return;
		}
		FPeer* Found = Peers.Find(Peer);
		if (Found == nullptr)
		{
			// Word goes ahead of the connection's first packet, which makes the place: kept for it. At most as many as there are places,
			// each for as long as a place waits to be let in, so strangers' words never push out a player's for long.
			const double Now = FPlatformTime::Seconds();
			const double Given = NotLetInWithinForTesting > 0.0 ? NotLetInWithinForTesting : NotLetInWithinSeconds;
			for (auto It = WordsBeforeTheirPlace.CreateIterator(); It; ++It)
			{
				if (Now - It.Value().Value >= Given)
				{
					It.RemoveCurrent();
				}
			}
			if (WordsBeforeTheirPlace.Contains(Peer) || WordsBeforeTheirPlace.Num() < (MostPlayersForTesting > 0 ? MostPlayersForTesting : MostPlayers))
			{
				WordsBeforeTheirPlace.Add(Peer, TPair<uint32, double>(Number, Now));
			}
			return;
		}
		// The place's own connection told again, or the first word heard for it: nothing moves.
		const bool bAnEarlierCameThrough = Found->Connection.IsSet() && Found->Connection.GetValue() != Number;
		Found->Connection = Number;
		if (!bAnEarlierCameThrough)
		{
			return;
		}
		// The engine hands a new connection from an address an earlier one still has to that one, until it times out: the new one
		// comes from a new loopback port, and the earlier one's closes (the engine drops a connection whose port went, measured).
		FFlockRelayFailure Failure;
		const TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Made = NewLoopback(Peer, Failure);
		Ending = Found->Loopback;
		if (Made.IsValid())
		{
			Made->SetLink(Found->Link);
			Found->Loopback = Made;
			Found->bWasLetIn = false;
			Found->WaitingSince = FPlatformTime::Seconds();
		}
		else
		{
			// No socket or thread now: the place is given back, and the connection's next packets make another.
			LinkClosing = Found->Link;
			if (LinkClosing.IsValid())
			{
				LinksLetGo.Add(LinkClosing);
			}
			Peers.Remove(Peer);
		}
	}
	if (LinkClosing.IsValid())
	{
		LinkClosing->Close();
	}
	Ending->End();
	Relay->LetChannelGo(Peer);
}

void FFlockRelayBridge::HandToTheEngineFrom(const FFlockRelayPeer& Peer, const uint8* Data, int32 Count) const
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
			Loopback = Peer == Host ? HostLoopback : nullptr;
		}
		else if (const FPeer* Found = Peers.Find(Peer))
		{
			Loopback = Found->Loopback;
		}
	}
	if (Loopback.IsValid())
	{
		Loopback->HandToTheEngine(Data, Count);
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

bool FFlockRelayBridge::GoesDirectForTesting() const
{
	FScopeLock Locked(&Gate);
	return bGoesDirect;
}

bool FFlockRelayBridge::HearsThatItReadsBridgeMessagesForTesting(const FFlockRelayPeer& Peer) const
{
	FScopeLock Locked(&Gate);
	return PeersReadingBridgeMessages.Contains(Peer);
}

FFlockDirectLink::FPtr FFlockRelayBridge::GetDirectLinkForTesting(const FFlockRelayPeer& Peer) const
{
	FScopeLock Locked(&Gate);
	if (!bForTheHost)
	{
		return Peer == Host ? HostLink : nullptr;
	}
	const FPeer* Found = Peers.Find(Peer);
	return Found != nullptr ? Found->Link : nullptr;
}
