// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Multiplayer/Relay/FlockRelayConnection.h"
#include "Multiplayer/Relay/FlockRelayFailures.h"
#include "Multiplayer/Relay/FlockRelayPackets.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"

class FFlockRelayLoopback;
struct FFlockRelayBridgeShared;

/**
 * Hands relayed packets to the engine's own net driver through this device's loopback, so a game sends and receives through
 * Flock's relay as on a direct connection: the net driver keeps its own socket, its handshake and its travel.
 *
 * A player's bridge is one loopback socket standing in for the host: the game travels to 127.0.0.1 on its port, what it sends
 * goes to the host's relay address, and only what the host sends through the relay reaches it. It follows the port the game
 * last sent from, since the net driver sends from a new port each time it connects (a reconnect after the host changed level
 * included).
 *
 * A host's bridge gives each relayed peer a loopback socket of its own that sends to the port the net driver listens on, so the
 * engine sees each player as a connection of its own (from 127.0.0.1 on that socket's port); at most 64, and a peer the host's
 * check has not let in within 30 s gives its place back when the next new peer is heard, so strangers on the relay cannot hold
 * every place. A channel on the relay is kept only for a peer let in.
 *
 * Each loopback socket has a thread blocked on it; the relay's thread hands arrivals straight to the right socket. One bridge a
 * relay: two games behind one relay address would reach the host as one. Game thread, but for what the relay's thread and the
 * loopback threads call. Exported, as the relay connection is, for the harness's real-player probe; the header stays private.
 */
class FLOCK_API FFlockRelayBridge : public IFlockRelayReceiver, public TSharedFromThis<FFlockRelayBridge, ESPMode::ThreadSafe>
{
public:
	/** The most relayed players a host's bridge forwards for at once, as many as the relay keeps channels for. */
	static constexpr int32 MostPlayers = FFlockRelayPackets::MostChannels;
	/** A relayed peer the host's check has not let in within this gives its place back when the next new peer is heard. */
	static constexpr double NotLetInWithinSeconds = 30.0;
	static constexpr const TCHAR* ThreadName = TEXT("Flock relay loopback");
	static constexpr uint32 ThreadStackBytes = 64 * 1024;

	using FRef = TSharedRef<FFlockRelayBridge, ESPMode::ThreadSafe>;
	using FPtr = TSharedPtr<FFlockRelayBridge, ESPMode::ThreadSafe>;

	/**
	 * A player's bridge: the game travels to 127.0.0.1 on GetLocalPort, and its packets go to Host's relay address, on a channel
	 * kept from the start. Fails relay_no_thread where no thread can be started, and with a reason-less Network or Validation
	 * failure when no loopback socket can be made or the relay already hands its packets to another receiver.
	 */
	static TFlockRelayResult<FPtr> ToTheHost(const TSharedRef<FFlockRelayConnection>& Relay, const FFlockRelayPeer& Host,
		const TSharedRef<IFlockLogger>& Logger);

	/** A host's bridge: each relayed peer gets a loopback socket of its own that sends to the net driver on NetDriverPort. */
	static TFlockRelayResult<FPtr> ForTheHost(const TSharedRef<FFlockRelayConnection>& Relay, int32 NetDriverPort,
		const TSharedRef<IFlockLogger>& Logger);

	virtual ~FFlockRelayBridge() override;

	FFlockRelayBridge(const FFlockRelayBridge&) = delete;
	FFlockRelayBridge& operator=(const FFlockRelayBridge&) = delete;

	/** The loopback port a player's game travels to; 0 on a host's bridge. */
	int32 GetLocalPort() const;

	/** The relay address a player's bridge stands in for; empty on a host's bridge. */
	FFlockRelayPeer GetHost() const { return Host; }

	bool IsForTheHost() const { return bForTheHost; }

	const TSharedRef<FFlockRelayConnection>& GetRelay() const { return Relay; }

	bool IsClosed() const;

	/** The host's net driver listens on Port now (a level opened again may bind another); 0 is ignored. */
	void SetNetDriverPort(int32 Port);

	/** Which relayed peer an engine connection from 127.0.0.1 on LoopbackPort is; false when it is none of this host bridge's. */
	bool TryGetPeer(int32 LoopbackPort, FFlockRelayPeer& OutPeer) const;

	/** The loopback port Peer reaches the engine from, or 0 when the bridge forwards nothing for it. */
	int32 LoopbackPortOf(const FFlockRelayPeer& Peer) const;

	/** The host's check let Peer in: it keeps its place, and its packets ride on a relay channel (4 bytes each rather than 36). */
	void LetIn(const FFlockRelayPeer& Peer);

	/** Forgets a relayed player that left or was refused, and stops renewing its channel; it gets a new place if it sends again. */
	void Forget(const FFlockRelayPeer& Peer);

	/**
	 * The player's let-in connection left while another of its connections still comes through this address (its game connected
	 * again): the address keeps its loopback socket, lets go of its channel, and waits to be let in again as a newly heard peer does.
	 */
	void NoLongerLetIn(const FFlockRelayPeer& Peer);

	/** Says what the loopback threads found for the game (a packet too large for the relay, once); run once a frame. */
	void Tick();

	/** Stops forwarding, closes every loopback socket, stops renewing the channels it kept, and hands arrivals back to the relay, which stays the session's. */
	void Close();

	/** The relay's thread: a packet a relayed peer sent, handed to the engine from that peer's loopback socket. */
	virtual void Arrived(const FFlockRelayPeer& From, const uint8* Data, int32 Count) override;

	/** Stands in for MostPlayers when above 0, so a test can fill a bridge with a few peers. */
	static int32 MostPlayersForTesting;
	/** Stands in for NotLetInWithinSeconds when above 0. */
	static double NotLetInWithinForTesting;
	/** Makes each loopback socket fail to be made. */
	static bool bSocketRefusedForTesting;

	int32 GetPeersForTesting() const;
	TArray<int32> GetPeerPortsForTesting() const;
	int32 GetNetDriverPortForTesting() const;

private:
	FFlockRelayBridge(const TSharedRef<FFlockRelayConnection>& InRelay, const TSharedRef<IFlockLogger>& InLogger, bool bInForTheHost,
		const FFlockRelayPeer& InHost);

	static TFlockRelayResult<FPtr> Register(const FRef& Bridge);

	struct FPeer
	{
		TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Loopback;
		/** When it began waiting to be let in: when made, or when no longer let in. */
		double WaitingSince = 0.0;
		bool bWasLetIn = false;
	};

	/** A loopback socket and its thread for Peer, or null with OutFailure said. */
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> NewLoopback(const FFlockRelayPeer& Peer, FFlockRelayFailure& OutFailure) const;

	/** Under Gate, as a new peer is first heard: peers not let in within their time (refused, or strangers) give their place back. */
	void ForgetPeersNeverLetIn(double Now);

	const TSharedRef<FFlockRelayConnection> Relay;
	const TSharedRef<IFlockLogger> Logger;
	const bool bForTheHost;
	const FFlockRelayPeer Host;
	/** What the loopback threads share with the bridge: the net driver's port, and what they found for the game. */
	const TSharedRef<FFlockRelayBridgeShared, ESPMode::ThreadSafe> Shared;

	mutable FCriticalSection Gate;
	/** A host's: one loopback socket a relayed peer, made when the peer is first heard. */
	TMap<FFlockRelayPeer, FPeer> Peers;
	/** A player's: the one loopback socket the game travels to, standing in for the host. */
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> HostLoopback;
	bool bClosed = false;
	bool bToldTooLarge = false;
};
