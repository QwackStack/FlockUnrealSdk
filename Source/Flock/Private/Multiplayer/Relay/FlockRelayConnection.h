// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockLogger.h"
#include "Http/FlockHttpAdapter.h"
#include "Multiplayer/FlockNameLookup.h"
#include "Multiplayer/Relay/FlockRelayFailures.h"
#include "Multiplayer/Relay/FlockRelayLogin.h"
#include "Multiplayer/Relay/FlockRelayPackets.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"

class FFlockRelayCore;

/** A STUN server Flock listed, being looked up (or looked up already), and its port. */
struct FFlockRelayStunLookUp
{
	TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> LookUp;
	int32 Port = FFlockRelayLogin::DefaultPort;
};

/** How long the relay's steps may take and how often it is renewed; a test shortens them. */
struct FFlockRelayTiming
{
	double NameLookUpWaitSeconds = 5.0;
	double AnswerWaitSeconds = 5.0;
	/** A relay server may answer only some of a device's ports, so the reservation is made from one of this many sockets it answered, a round at a time. */
	int32 SocketsTriedAtOnce = 8;
	int32 RoundsOfSocketsTried = 3;
	double SocketRoundWaitSeconds = 1.0;
	/** The reservation is renewed at half the time the relay grants, and at least this often. */
	double LongestBetweenReservationRenewalsSeconds = 240.0;
	double RenewOpeningsEverySeconds = 240.0;
	double RenewChannelsEverySeconds = 480.0;
	double RetryRenewalAfterSeconds = 15.0;
	/**
	 * The relay is asked this often (a binding request), whatever else goes: the game's packets need no answer, so they say nothing
	 * of whether the relay still hears this device, and the ask keeps the way to the relay open through a router too.
	 */
	double AskTheRelayEverySeconds = 15.0;
	/**
	 * A relay that answered the asks and has answered nothing for this long is lost: this device's packets no longer reach it (a
	 * router that moved this device to another port, a network that changed), though the relay's may still arrive.
	 */
	double RelayLostWhenSilentSeconds = 45.0;
	/** How long the relay keeps an opening and a channel without a renewal, as the standard sets them. */
	double OpeningLastsSeconds = 300.0;
	double ChannelLastsSeconds = 600.0;
	/** How long a closed connection waits for the release's answer, to send it again when the relay's nonce went stale. */
	double ReleaseAnswerWaitSeconds = 1.0;
	FFlockNameLookup::FLookUp NameLookUpForTesting;
};

/**
 * One address on Flock's relay for one session, kept alive on a thread of its own ("Flock relay"); it hands raw packets over
 * without waiting on the relay. Its answers reach the game on the game thread, from Tick, which its owner runs once a frame.
 * Game thread, but for Send, TryReceive, KeepChannelTo, LetChannelGo and the arrivals' hand-over, which any thread may call.
 * Exported for the engine plug-in and the harness's real-player probe; the header stays private.
 */
class FLOCK_API FFlockRelayConnection : public TSharedFromThis<FFlockRelayConnection>
{
public:
	/** The most bytes one packet may carry. */
	static constexpr int32 MostBytesInAPacket = FFlockRelayPackets::MostBytesInAPacket;
	/** The most IPs one relay address is opened to: one for each of the 64 players a host takes through the relay, and the host's own. */
	static constexpr int32 MostOpenings = 65;
	static constexpr const TCHAR* ThreadName = TEXT("Flock relay");

	using FOnOpened = TFunction<void(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>>)>;

	/**
	 * Reserves an address on the relay with Login, on a thread of its own. OnOpened answers once, on the game thread: the
	 * connection open, or why not; Close before then answers it Cancelled. It is answered once the STUN servers being looked up
	 * are, or the lookup's wait is over; one that could not be looked up is left out.
	 */
	static TSharedRef<FFlockRelayConnection> Open(const FFlockRelayLogin& Login, const TArray<FFlockRelayStunLookUp>& StunServers,
		const FFlockRelayTiming& Timing, const TSharedRef<IFlockLogger>& Logger, FOnOpened OnOpened);

	/** Closes the relay, if its owner had not. */
	~FFlockRelayConnection();

	FFlockRelayConnection(const FFlockRelayConnection&) = delete;
	FFlockRelayConnection& operator=(const FFlockRelayConnection&) = delete;

	/** Hands the game what the relay's thread has for it: the opening's answer, openings answered, warnings. Run once a frame. */
	void Tick();

	/** This player's address on the relay; the host publishes it, and players send to it. Empty until open. */
	FFlockRelayPeer GetAddress() const;

	/** The relay server's IPv4 address and port; it also answers a STUN request with the address it saw the request come from. Empty until looked up. */
	FFlockRelayPeer GetServerEndpoint() const;

	/** The relay server this address is on, as "name:port" from the login Flock minted; players reserve on the server the host uses. */
	FString GetServer() const;

	/** The STUN servers Flock listed with the login, looked up (one that could not be is left out); a direct link asks them too. Set before the opening is answered. */
	TArray<FFlockRelayPeer> GetStunServers() const;

	bool IsOpen() const;

	/** True once closed or failed; nothing more is sent. */
	bool HasStopped() const;

	/** One of FlockRelayFailureReasons once the relay failed or was lost; empty while it works and after a plain close. */
	FString GetFailureReason() const;

	/** Packets that arrived while room for 256 was full, or that were larger than the most. */
	int64 GetPacketsDropped() const;

	/** Packets sent on through the relay: channel frames and send indications. */
	int64 GetPacketsSent() const;

	/**
	 * Lets packets in from, and out to, every relay address sharing Address's IP, renewed until closed: the host's address for
	 * everyone, and each seated player's for the host, since a relay may give each address an IP of its own. A call for an IP
	 * being asked waits for that request; one the relay refused is refused again without asking; past MostOpenings IPs, refused
	 * without asking. Cancelling the handle answers this call alone Cancelled; the request goes on, and what it opens is kept.
	 */
	FFlockRequestHandle OpenTo(const FFlockRelayPeer& Address, TFunction<void(TFlockRelayResult<bool>)> OnOpen);

	/** True while the relay lets packets in from Address's IP. */
	bool IsOpenTo(const FFlockRelayPeer& Address) const;

	/** True once the relay refused to open to Address's IP; it is not asked again. */
	bool RefusedToOpenTo(const FFlockRelayPeer& Address) const;


	/** Sends one packet to To through the relay; false when the relay is not open, the packet is empty or over MostBytesInAPacket, or To's IP was never opened. Any thread. */
	bool Send(const FFlockRelayPeer& To, const uint8* Data, int32 Count);

	/** Takes the oldest packet that arrived, copied into Into (room for at least MostBytesInAPacket bytes); false when none is waiting. Any thread. */
	bool TryReceive(uint8* Into, int32 IntoSize, int32& OutCount, FFlockRelayPeer& OutFrom);

	/** Packets to and from Peer ride on a relay channel (4 bytes each rather than 36), renewed until closed. Asked for the host by a player, and by the host for each player it let in. Any thread. */
	void KeepChannelTo(const FFlockRelayPeer& Peer);

	/** Stops renewing the channel to Peer, so the relay lets it lapse (a channel cannot be unbound); KeepChannelTo renews it again on the same number. Any thread. */
	void LetChannelGo(const FFlockRelayPeer& Peer);

	/** Hands every packet that arrives to Receiver on the relay's thread, which it must not hold; false when another receiver has them. Any thread. */
	bool HandArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& Receiver);

	/** Keeps arrivals for TryReceive again, if Receiver is the one they are handed to. Any thread. */
	void StopHandingArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& Receiver);

	/** True while a receiver hears what arrives, rather than the store TryReceive reads. */
	bool HandsArrivalsOver() const;

	/** Gives the address back (sent from the calling thread, so it goes out even while the game quits) and stops; a call still waiting ends Cancelled. */
	void Close();

	/**
	 * Every relay thread still running (each relay connection's, and each bridge loopback socket's) is told to stop at once and
	 * waited for: Flock's module is going away. The release each relay needed already went out when it was closed.
	 */
	static void StopEveryRelayThread();

	/** Relay threads not yet finished and cleaned up. */
	static int32 GetRelayThreadsForTesting();

	/** Stands in for MostOpenings when above 0, so a test can reach it with a few addresses. */
	static int32 MostOpeningsForTesting;

	int32 GetNoncesTakenForTesting() const;
	int32 GetLocalPortForTesting() const;
	FName GetSocketProtocolForTesting() const;
	bool KeepsPacketsForTesting() const;
	bool KeepsChannelToForTesting(const FFlockRelayPeer& Peer) const;
	uint16 ChannelNumberForTesting(const FFlockRelayPeer& Peer) const;
	int32 GetOpeningsAskedForTesting() const { return Asked.Num(); }
	/** The relay's thread, once it ran. */
	uint32 GetThreadIdForTesting() const;

private:
	friend class FFlockRelayCore;

	FFlockRelayConnection(const TSharedRef<FFlockRelayCore, ESPMode::ThreadSafe>& InCore, const TSharedRef<IFlockLogger>& InLogger, FOnOpened InOnOpened);

	struct FOpeningWaiter
	{
		uint32 Ip = 0;
		TFunction<void(TFlockRelayResult<bool>)> OnOpen;
	};
	using FOpeningWaiterRef = TSharedRef<FOpeningWaiter>;

	/** The reason this connection is not open, for a call made after it stopped. */
	FFlockRelayFailure NotOpen() const;
	void AnswerOpened(TFlockRelayResult<TSharedPtr<FFlockRelayConnection>> Result);
	void AnswerOpeningWaiters(uint32 Ip, const TFlockRelayResult<bool>& Result);
	void AnswerEveryWaiter(const TFlockRelayResult<bool>& Result);

	TSharedRef<FFlockRelayCore, ESPMode::ThreadSafe> Core;
	TSharedRef<IFlockLogger> Logger;
	FOnOpened OnOpened;
	/** Openings asked and not yet answered, so a second caller waits for the first; and IPs the relay refused, not asked again. */
	TSet<uint32> Asked;
	TSet<uint32> Refused;
	TArray<FOpeningWaiterRef> Waiters;
};
