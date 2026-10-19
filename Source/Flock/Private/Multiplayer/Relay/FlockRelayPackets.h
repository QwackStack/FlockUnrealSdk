// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"
#include "Multiplayer/Relay/FlockRelayWire.h"
#include <atomic>

class FFlockRelaySocket;

/** Hears every packet that arrives through the relay, on the relay's thread, which it must not hold. */
class IFlockRelayReceiver
{
public:
	virtual ~IFlockRelayReceiver() = default;
	virtual void Arrived(const FFlockRelayPeer& From, const uint8* Data, int32 Count) = 0;
};

/**
 * The packets a relay connection carries: the IPs the relay opened to, the channel bound to each peer, sending, and what
 * arrived waiting for the game. Sends and reads of what arrived from any thread, without allocating; the rest as each member says.
 */
class FFlockRelayPackets
{
public:
	/** The most bytes one packet may carry. */
	static constexpr int32 MostBytesInAPacket = 1400;
	static constexpr int32 PacketsHeld = 256;
	static constexpr int32 MostChannels = 64;

	explicit FFlockRelayPackets(FFlockRelaySocket& InSocket);

	/** Packets that arrived while room for 256 was full, or that were larger than the most. */
	int64 GetPacketsDropped() const { return PacketsDropped.load(); }

	/** Packets sent on through the relay: channel frames and send indications. */
	int64 GetPacketsSent() const { return PacketsSent.load(); }

	bool IsOpenedTo(uint32 Ip) const;

	/** How many IPs the relay opened to. */
	int32 GetOpeningsCount() const;

	/** Notes an IP the relay opened to; false when it already was. The relay's thread alone. */
	bool AddOpening(uint32 Ip);

	/** Sends one packet to To, on its channel once one is bound; false when it is empty, over the most, to an IP never opened, or the socket refused it. */
	bool Send(const FFlockRelayPeer& To, const uint8* Data, int32 Count);

	/** Hands every packet that arrives to Receiver, on the relay's thread, instead of keeping it; false when another receiver has them. */
	bool HandArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& Receiver);

	/** Keeps arrivals again, if Receiver is the one they are handed to. */
	void StopHandingArrivalsTo(const TSharedRef<IFlockRelayReceiver, ESPMode::ThreadSafe>& Receiver);

	bool HandsArrivalsOver() const;

	bool KeepsPacketsForTesting() const;

	/** Takes the oldest packet that arrived, copied into Into (room for at least MostBytesInAPacket bytes); false when none is waiting or Into is too small. */
	bool TryReceive(uint8* Into, int32 IntoSize, int32& OutCount, FFlockRelayPeer& OutFrom);

	/** Keeps a packet a peer sent through the relay, a channel frame or a data indication; false when it is neither, so it is the relay's own message. The relay's thread alone. */
	bool TryTake(const uint8* Packet, int32 Length);

	/** Gives the next peer a channel was wanted for its number, once each; false when there is none new. The relay's thread alone. */
	bool TryGiveNextChannel(FFlockRelayPeer& OutPeer, uint16& OutChannel);

	/** Packets to Peer go on Number from now. The relay's thread alone. */
	void ChannelBound(const FFlockRelayPeer& Peer, uint16 Number);

	/** The relay let Peer's channel go: packets to it go as send indications. The relay's thread alone. */
	void ChannelLetGo(const FFlockRelayPeer& Peer);

	/** The channel number packets to Peer go on now; 0 when none is bound. Any thread. */
	uint16 ChannelFor(const FFlockRelayPeer& Peer) const;

	/**
	 * Binds a channel to Peer on the relay's thread's next turn, once each; past the most channels a relay connection keeps,
	 * packets go as send indications. Only a peer known to be a player is given one, so a stranger on the relay cannot take
	 * them all. Any thread.
	 */
	void WantChannel(const FFlockRelayPeer& Peer);

	/** Stops renewing Peer's channel, so the relay lets it lapse; nothing for a peer no channel was wanted for. Any thread. */
	void LetChannelGo(const FFlockRelayPeer& Peer);

	/** Whether Peer's channel is renewed: wanted and not let go since. Any thread. */
	bool KeepsChannelTo(const FFlockRelayPeer& Peer) const;

private:
	struct FChannel
	{
		FFlockRelayPeer Peer;
		uint16 Number = 0;
	};

	uint16 ChannelForLocked(const FFlockRelayPeer& Peer) const;
	void Arrive(const FFlockRelayPeer& From, const uint8* Packet, int32 Offset, int32 Count);

	FFlockRelaySocket& Socket;

	/** The IPs opened to, the channels bound and the send buffer. */
	mutable FCriticalSection StateLock;
	TArray<uint32> OpenedTo;
	TArray<FChannel> Channels;
	uint8 SendBuffer[FFlockRelayWire::SendIndicationOverhead + MostBytesInAPacket + 4];
	uint8 IndicationTransaction[FFlockRelayWire::TransactionLength];
	uint32 IndicationsSent = 0;

	/** Every peer a channel was ever wanted for, and those no longer wanted (a player that left), from any thread. */
	mutable FCriticalSection WantedLock;
	TArray<FFlockRelayPeer> ChannelsWanted;
	TArray<FFlockRelayPeer> ChannelsLetGo;

	/** The relay's thread's own: the peer each channel number went to, and how many went. */
	FFlockRelayPeer PeerOfChannel[MostChannels];
	int32 ChannelsAsked = 0;

	/** Who packets that arrive are handed to on the relay's thread, at most one; none keeps them in the store below. */
	mutable FCriticalSection ReceiverLock;
	/** Held weakly, so a receiver its owner dropped hands the arrivals back by itself (a receiver holds the relay). */
	TWeakPtr<IFlockRelayReceiver, ESPMode::ThreadSafe> Receiver;

	/** Packets that arrived, waiting for the game: one block of slots, made when the first is kept. */
	mutable FCriticalSection ArrivedLock;
	TArray<uint8> ArrivedBytes;
	int32 ArrivedLengths[PacketsHeld];
	FFlockRelayPeer ArrivedFrom[PacketsHeld];
	int32 ArrivedFirst = 0;
	int32 ArrivedCount = 0;

	std::atomic<int64> PacketsDropped{ 0 };
	std::atomic<int64> PacketsSent{ 0 };
};
