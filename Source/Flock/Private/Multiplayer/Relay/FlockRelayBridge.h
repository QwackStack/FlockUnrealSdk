// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Multiplayer/Relay/FlockDirectLink.h"
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
 * Direct paths (Relay When Needed): the engine's packets to a peer move to a direct link (FFlockDirectLink) once the two ends
 * reach each other, and back to the relay when it goes quiet; the engine hears the peer from the same loopback port throughout. A
 * player's bridge makes a link for each connection its game makes; a host's makes one for each player its check let in, and a new
 * one when that player's offer names a new key (a new link behind it).
 *
 * The bridges' own messages (FFlockBridgeMessages) never reach the engine, which closes a connection any of them reaches
 * (measured), and are sent only to a bridge that said it reads them. A player's bridge says when its game starts a new
 * connection; the host's gives that peer a new loopback port, since the engine hands a new connection from an address its old
 * one still has to the old one until that one times out (measured: a minute).
 *
 * Each loopback socket and each link has a thread blocked on its socket; the relay's thread hands arrivals straight to the right
 * one. One bridge a relay: two games behind one relay address would reach the host as one. Game thread, but for what the relay's
 * thread, the loopback threads and the links call. Exported, as the relay connection is, for the harness's real-player probe; the
 * header stays private.
 */
class FLOCK_API FFlockRelayBridge : public IFlockRelayReceiver, public TSharedFromThis<FFlockRelayBridge, ESPMode::ThreadSafe>
{
public:
	/** The most relayed players a host's bridge forwards for at once, as many as the relay keeps channels for. */
	static constexpr int32 MostPlayers = FFlockRelayPackets::MostChannels;
	/** A relayed peer the host's check has not let in within this gives its place back when the next new peer is heard. */
	static constexpr double NotLetInWithinSeconds = 30.0;
	/** Word of a new connection goes ahead of this many of its first packets, so a lost one is made up for. */
	static constexpr int32 NewConnectionToldTimes = 3;
	static constexpr const TCHAR* ThreadName = TEXT("Flock relay loopback");
	static constexpr uint32 ThreadStackBytes = 64 * 1024;

	using FRef = TSharedRef<FFlockRelayBridge, ESPMode::ThreadSafe>;
	using FPtr = TSharedPtr<FFlockRelayBridge, ESPMode::ThreadSafe>;

	/** What a player's bridge does beyond carrying the engine's packets. */
	struct FToTheHostOptions
	{
		/** The host's bridge reads the bridges' own messages (it published so); none is sent to one that did not. */
		bool bHostReadsBridgeMessages = false;
		/** The engine's packets move to a direct path once the host and this device reach each other (Relay When Needed, a host that offers one). */
		bool bGoesDirect = false;
	};

	/**
	 * A player's bridge: the game travels to 127.0.0.1 on GetLocalPort, and its packets go to Host's relay address, on a channel
	 * kept from the start. Fails relay_no_thread where no thread can be started, and with a reason-less Network or Validation
	 * failure when no loopback socket can be made or the relay already hands its packets to another receiver.
	 */
	static TFlockRelayResult<FPtr> ToTheHost(const TSharedRef<FFlockRelayConnection>& Relay, const FFlockRelayPeer& Host,
		const TSharedRef<IFlockLogger>& Logger, const FToTheHostOptions& Options = FToTheHostOptions());

	/**
	 * A host's bridge: each relayed peer gets a loopback socket of its own that sends to the net driver on NetDriverPort. With
	 * bGoesDirect, each player let in whose seat says it reads the bridges' messages is offered a direct path.
	 */
	static TFlockRelayResult<FPtr> ForTheHost(const TSharedRef<FFlockRelayConnection>& Relay, int32 NetDriverPort,
		const TSharedRef<IFlockLogger>& Logger, bool bGoesDirect = false);

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

	/**
	 * A player's: its game is connecting to the host again (the engine's own connection back, as Travel To Session sees it), so the
	 * packets from now on are a new connection's: the host's bridge is told, and the new connection gets a link of its own. A new port
	 * the engine sends from without this is a socket it remade for the same connection, which keeps its place and its link.
	 */
	void GameConnectsAgain();

	/**
	 * A host's bridge offers direct paths from now on (the host travelled again with Relay When Needed), or no longer does (Always):
	 * then every link is closed and the engine's packets go through the relay.
	 */
	void SetGoesDirect(bool bInGoesDirect);

	/** The relay addresses whose seats say their bridges read the bridges' own messages, from the session's latest reading; a host's bridge offers direct paths to those alone. */
	void SetPeersThatReadBridgeMessages(const TSet<FFlockRelayPeer>& InPeers);

	/** Which relayed peer an engine connection from 127.0.0.1 on LoopbackPort is; false when it is none of this host bridge's. */
	bool TryGetPeer(int32 LoopbackPort, FFlockRelayPeer& OutPeer) const;

	/** The loopback port Peer reaches the engine from, or 0 when the bridge forwards nothing for it. */
	int32 LoopbackPortOf(const FFlockRelayPeer& Peer) const;

	/**
	 * The host's check let Peer in: it keeps its place, its packets ride on a relay channel (4 bytes each rather than 36), and it is
	 * offered a direct path when the bridge goes direct and Peer reads the bridges' messages.
	 */
	void LetIn(const FFlockRelayPeer& Peer);

	/** Forgets a relayed player that left or was refused, stops renewing its channel and closes its link; it gets a new place if it sends again. */
	void Forget(const FFlockRelayPeer& Peer);

	/**
	 * The player's let-in connection left while another of its connections still comes through this address (its game connected
	 * again): the address keeps its loopback socket and its link, lets go of its channel, and waits to be let in again as a newly
	 * heard peer does.
	 */
	void NoLongerLetIn(const FFlockRelayPeer& Peer);

	/** Says what the loopback threads and the links found for the game (a packet too large for the relay, once; each move to and from a direct path); run once a frame. */
	void Tick();

	/** Stops forwarding, closes every loopback socket and link, stops renewing the channels it kept, and hands arrivals back to the relay, which stays the session's. */
	void Close();

	/** The relay's thread: a packet a relayed peer sent, handed to the engine from that peer's loopback socket; the bridges' own messages never are. */
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
	/** The direct link to Peer (the host, on a player's bridge), or null when there is none. */
	FFlockDirectLink::FPtr GetDirectLinkForTesting(const FFlockRelayPeer& Peer) const;
	bool GoesDirectForTesting() const;
	/** A player's: told its game connects again, and no packet of the new connection has come since. */
	bool IsToldTheGameConnectsAgainForTesting() const;
	/** True when the host's bridge was told Peer's seat says its bridge reads the bridges' messages. */
	bool HearsThatItReadsBridgeMessagesForTesting(const FFlockRelayPeer& Peer) const;

private:
	FFlockRelayBridge(const TSharedRef<FFlockRelayConnection>& InRelay, const TSharedRef<IFlockLogger>& InLogger, bool bInForTheHost,
		const FFlockRelayPeer& InHost, bool bInGoesDirect);

	static TFlockRelayResult<FPtr> Register(const FRef& Bridge);

	struct FPeer
	{
		TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> Loopback;
		FFlockDirectLink::FPtr Link;
		/** When it began waiting to be let in: when made, or when no longer let in. */
		double WaitingSince = 0.0;
		bool bWasLetIn = false;
		/** The number of the connection the peer's bridge last said its game started; unset before it said one. */
		TOptional<uint32> Connection;
	};

	/** A loopback socket and its thread for Peer, or null with OutFailure said. */
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> NewLoopback(const FFlockRelayPeer& Peer, FFlockRelayFailure& OutFailure) const;

	/** A direct link to Peer, handing what arrives to the engine from Peer's loopback socket of the moment; null when none can be started. */
	FFlockDirectLink::FPtr NewLink(const FFlockRelayPeer& Peer) const;

	/** Under Gate: a host's bridge offers Peer a direct path once let in, when it goes direct and Peer reads the bridges' messages. */
	bool OffersADirectPathTo(const FFlockRelayPeer& Peer, const FPeer& Entry) const;

	/** A host's: a link for Peer, kept if Peer still has its place, no link and the offer of one meanwhile. */
	void GiveALink(const FFlockRelayPeer& Peer);

	/** A host's: a new link for Peer in place of Old, if Old is still its link; the link to hand the offer to, or null. */
	FFlockDirectLink::FPtr ReplaceLink(const FFlockRelayPeer& Peer, const FFlockDirectLink::FPtr& Old);

	/** Under Gate, as a new peer is first heard: peers not let in within their time (refused, or strangers) give their place back. */
	void ForgetPeersNeverLetIn(double Now, TArray<FFlockDirectLink::FPtr>& OutLinksClosing);

	/** The relay's thread: an offer, or word of going back to the relay or of a new connection, from the bridge at the other end. */
	void HeardFromTheOtherBridge(const FFlockRelayPeer& From, const uint8* Data, int32 Count);

	/** The relay's thread: Peer's game started connection Number; a place an earlier connection of another number came through gets a new loopback port. */
	void HeardOfANewConnection(const FFlockRelayPeer& Peer, uint32 Number);

	/** The link's thread: a packet Peer sent directly, handed to the engine from Peer's loopback socket. */
	void HandToTheEngineFrom(const FFlockRelayPeer& Peer, const uint8* Data, int32 Count) const;

	const TSharedRef<FFlockRelayConnection> Relay;
	const TSharedRef<IFlockLogger> Logger;
	const bool bForTheHost;
	const FFlockRelayPeer Host;
	/** What the loopback threads share with the bridge: the net driver's port, and what they found for the game. */
	const TSharedRef<FFlockRelayBridgeShared, ESPMode::ThreadSafe> Shared;

	mutable FCriticalSection Gate;
	bool bGoesDirect = false;
	/** A host's: one loopback socket a relayed peer, made when the peer is first heard. */
	TMap<FFlockRelayPeer, FPeer> Peers;
	/** A host's: the peers whose seats say they read the bridges' messages. */
	TSet<FFlockRelayPeer> PeersReadingBridgeMessages;
	/** A host's: word of a new connection heard from a peer before its first packet made its place, kept for that place: its number and when. */
	TMap<FFlockRelayPeer, TPair<uint32, double>> WordsBeforeTheirPlace;
	/** A player's: the one loopback socket the game travels to, standing in for the host. */
	TSharedPtr<FFlockRelayLoopback, ESPMode::ThreadSafe> HostLoopback;
	/** A player's: its link to the host, for the connection its game makes now. */
	FFlockDirectLink::FPtr HostLink;
	/** A player's: how many of its game's connections have had a link of their own. */
	int32 ConnectionsLinked = 0;
	/** Links let go of since the last frame, whose last lines are still to be said. */
	TArray<FFlockDirectLink::FPtr> LinksLetGo;
	/** The game thread's own, kept between frames: the links Tick takes lines from. */
	TArray<FFlockDirectLink::FPtr> Saying;
	bool bClosed = false;
	bool bToldTooLarge = false;
};
