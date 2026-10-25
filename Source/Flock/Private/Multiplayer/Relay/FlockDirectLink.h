// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/Relay/FlockBridgeMessages.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"
#include "Multiplayer/Relay/FlockRelayThreads.h"
#include <atomic>

class FSocket;
class FInternetAddr;

/**
 * Tries to reach one relayed peer directly, from a UDP socket of its own (dual-stack where the platform allows): learns the
 * address Flock's STUN servers or the relay server see it at, offers that and its LAN address through the relay, sends probes at
 * the peer's addresses while the peer sends at its own, and once a probe is answered carries the engine's packets directly. It
 * stays on the relay for good when no answer comes, and goes back to it when the direct path goes quiet; the relay stays reserved
 * and its channel kept, so going back loses nothing but the quiet time.
 *
 * Its own thread reads the socket, a tick at most apart, and keeps the timers; offers and word from the other end arrive from the
 * relay's thread; the engine's packets are sent from a loopback socket's thread. The socket is closed by its own thread when it
 * ends, never while another sends on it. What it says for the game is taken on the game thread (TakeLines).
 */
class FLOCK_API FFlockDirectLink : public IFlockRelayThreadWork, public TSharedFromThis<FFlockDirectLink, ESPMode::ThreadSafe>
{
public:
	struct FTiming
	{
		double TickSeconds = 0.05;
		double AskAddressEverySeconds = 0.5;
		int32 AddressAsks = 3;
		double OfferEverySeconds = 1.0;
		double OfferForSeconds = 20.0;
		double ProbeEverySeconds = 0.2;
		double ProbeForSeconds = 6.0;
		double KeepAliveEverySeconds = 1.0;
		double QuietForSeconds = 4.0;
	};

	using FPtr = TSharedPtr<FFlockDirectLink, ESPMode::ThreadSafe>;
	/** Sends one of the bridges' messages to the peer through the relay. Any thread. */
	using FSendThroughTheRelay = TFunction<void(const TArray<uint8>&)>;
	/** Hands a packet the peer sent directly to the engine, from the loopback socket standing in for the peer. The link's thread. */
	using FHandToTheEngine = TFunction<void(const uint8*, int32)>;

	static constexpr const TCHAR* ThreadName = TEXT("Flock direct link");
	static constexpr uint32 ThreadStackBytes = 64 * 1024;

	/**
	 * A link to the peer the relay knows as PeerName, started at once: it asks StunServers and RelayServer (unless empty, as for a
	 * relay reached over IPv6) where they see its socket, then offers through SendThroughTheRelay. Null when no socket can be opened
	 * or no thread started; the peer stays on the relay.
	 */
	static FPtr Start(const FFlockRelayPeer& RelayServer, const TArray<FFlockRelayPeer>& StunServers, FSendThroughTheRelay SendThroughTheRelay,
		FHandToTheEngine HandToTheEngine, const FString& PeerName);

	virtual ~FFlockDirectLink() override;

	FFlockDirectLink(const FFlockDirectLink&) = delete;
	FFlockDirectLink& operator=(const FFlockDirectLink&) = delete;

	/** True while the engine's packets to the peer go directly. */
	bool IsDirect() const;

	/** True once the link gave up, went back to the relay, or was closed: nothing more goes directly. */
	bool HasStopped() const;

	/** True when the link heard the peer's key and Key is another: the peer's bridge is not the one this link offered to. */
	bool HeardAnotherKeyThan(const FFlockBridgeKey& Key) const;

	/**
	 * The peer's offer, heard through the relay: its key, the key it holds for this link (unset for none) and its addresses. An offer
	 * without this link's key asks for this link's offer again (the last may have been lost). The relay's thread.
	 */
	void HeardOffer(const FFlockBridgeKey& TheirKey, const TOptional<FFlockBridgeKey>& KeyTheyHold, const TArray<FFlockRelayPeer>& TheirAddresses);

	/** The peer said, through the relay, that it went back to it: this link does too. The relay's thread. */
	void HeardBackToTheRelay(const uint8* Data, int32 Count);

	/** Sends one of the engine's packets directly; false when the link is not direct (or the send failed), so it goes through the relay. */
	bool TrySend(const uint8* Data, int32 Count);

	/** Nothing more goes directly; the thread closes the socket within a tick. Any thread. */
	void Close();

	/** What the link has said since last asked, for the game's log (Info). The game thread. */
	TArray<FString> TakeLines();

	/** True when the link has said something not yet taken; any thread, without a lock. */
	bool HasLines() const { return bHasLines.load(); }

	// ---- the link's own thread ----

	virtual uint32 Run() override;
	virtual bool HasEnded() const override { return bEnded.load(); }
	virtual void EndNow() override { Close(); }

	/** Stands in for every new link's timing, so a test runs in seconds. */
	static TOptional<FTiming> TimingForTesting;
	/** The next link made offers these addresses instead of its own, as an end whose router gives each destination its own port would. */
	static void OfferInsteadForTesting(const TArray<FFlockRelayPeer>& Addresses);

	int32 GetLocalPortForTesting() const { return LocalPort; }
	FName GetProtocolForTesting() const { return Protocol; }
	/** The address a server said it sees this link's socket at; unset before an answer. */
	TOptional<FFlockRelayPeer> GetPublicAddressForTesting() const;
	/** True while probes go out: both offers are known and no answer has come yet. */
	bool IsProbingForTesting() const;
	/** The addresses the peer offered and those its probes came from; empty before its offer. */
	TArray<FFlockRelayPeer> GetTheirAddressesForTesting() const;

	/** Drops everything that reaches the socket, as a path that died would. */
	std::atomic<bool> bHearsNothingForTesting{ false };
	/** Drops the peer's answers to this link's probes, as a path that works one way only would. */
	std::atomic<bool> bDropsAnswersForTesting{ false };
	/** Drops what comes from an address this link never sent to, as a home router does. */
	std::atomic<bool> bFiltersLikeARouterForTesting{ false };

private:
	enum class EStage : uint8 { LearningItsAddress, Offering, Probing, Direct, OnTheRelay, Closed };

	FFlockDirectLink(FSocket* InSocket, const FTiming& InTiming, const FFlockRelayPeer& InRelayServer, const TArray<FFlockRelayPeer>& StunServers,
		FSendThroughTheRelay InSendThroughTheRelay, FHandToTheEngine InHandToTheEngine, const FString& InPeerName);

	/** The peer's offered addresses and the ones its probes came from. */
	static constexpr int32 MostAddressesProbed = FFlockBridgeMessages::MostAddressesOffered + 2;

	void Heard(const uint8* Data, int32 Count, const FFlockRelayPeer& From);
	void HeardMessage(const uint8* Data, int32 Count, const FFlockRelayPeer& From);
	void Tick(double Now);
	/** Under Gate: this socket's addresses are as known as they will be, so they are offered. */
	void AddressesKnown(double Now);
	void StartProbing(double Now);
	/** Under Gate: offers again while the peer's offer has not come, and once at once when the peer asked by offering. */
	void OfferWhenDue(double Now, TArray<TArray<uint8>>& OutThroughTheRelay);
	void GiveUp(const TCHAR* Why);
	void StopGoingDirect(EStage To);
	/** Under Gate. Once direct the socket is connected to the peer, which Windows lets send only without an address, and only there. */
	void SendTo(const TArray<uint8>& Bytes, const FFlockRelayPeer& To);
	void Say(const FString& Line);
	void CloseSocket();

	const FTiming Timing;
	/** Where the socket's own address is asked: the STUN servers Flock lists, then the relay server. A relay server may answer only some of a device's ports (Cloudflare's answered about one in three, measured), and asking again from the same port changes nothing. */
	TArray<FFlockRelayPeer> AddressServers;
	const FSendThroughTheRelay SendThroughTheRelay;
	const FHandToTheEngine HandToTheEngine;
	const FString PeerName;
	const FFlockBridgeKey OurKey;
	const TArray<uint8> AddressTransaction;
	const TArray<uint8> AddressRequest;
	const int32 LocalPort;
	const FName Protocol;
	/** The address this device sends from toward the relay server; 0 when there is none. */
	uint32 LanAddress = 0;
	/** The thread's own: where a packet it read came from. */
	TSharedPtr<FInternetAddr> From;

	/** Guards the socket against being closed while another thread sends on it. */
	FCriticalSection SocketLock;
	FSocket* Socket = nullptr;

	mutable FCriticalSection Gate;
	EStage Stage = EStage::LearningItsAddress;
	TOptional<FFlockRelayPeer> PublicAddress;
	TArray<FFlockRelayPeer> OurAddresses;
	TArray<FFlockRelayPeer> OffersInstead;
	TOptional<FFlockBridgeKey> TheirKey;
	TArray<FFlockRelayPeer> TheirAddresses;
	/** Where a probe or answer naming both keys came from: its engine packets are taken; the answered one carries this link's own. */
	TOptional<FFlockRelayPeer> HeardAt;
	TOptional<FFlockRelayPeer> Direct;
	/** Everywhere this link sent, for a test that has it filter as a home router does. */
	TSet<FFlockRelayPeer> SentTo;
	bool bOfferNow = false;
	int32 AddressAsksMade = 0;
	double NextAddressAskAt = 0.0;
	double NextOfferAt = 0.0;
	double OfferUntil = 0.0;
	double NextProbeAt = 0.0;
	double ProbeUntil = 0.0;
	double NextKeepAliveAt = 0.0;
	double LastHeardAt = 0.0;

	FCriticalSection LinesLock;
	TArray<FString> Lines;
	std::atomic<bool> bHasLines{ false };
	std::atomic<bool> bEnded{ false };

	static FCriticalSection OffersInsteadLock;
	static TArray<FFlockRelayPeer> NextOffersInstead;
};
