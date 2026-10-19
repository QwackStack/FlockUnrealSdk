// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Multiplayer/Relay/FlockRelayPeer.h"

/** The key one end of a direct path names itself by in the bridges' messages: 8 bytes no one can guess. */
struct FLOCK_API FFlockBridgeKey
{
	static constexpr int32 Length = 8;

	uint8 Bytes[Length] = {};

	bool operator==(const FFlockBridgeKey& Other) const { return FMemory::Memcmp(Bytes, Other.Bytes, Length) == 0; }
	bool operator!=(const FFlockBridgeKey& Other) const { return !(*this == Other); }

	/** A new key from the platform's random source. */
	static FFlockBridgeKey NewRandom();
};

/**
 * The messages two relay bridges send each other: an offer of the addresses an end may be reached at, through the relay; then
 * probes, answers and keep-alives on the direct path; word that one end went back to the relay; and word that the game behind a
 * player's bridge started a new connection. Each starts with 0xFA and the letters "LNK", then the version and the kind.
 *
 * The engine closes a connection that any of them reaches, at the host and at the player alike (measured: 0xFA sets the stateless
 * handshake's bit, and the handshake that follows cannot be read). So a bridge keeps every one of them from the engine, whatever
 * its version, and sends them only to a bridge that said it reads them (FlockRelayConnectionValues::BridgeMessages). No packet of
 * the engine starts with them: a handshake packet's first two bytes carry its lowest handshake version, which would read 153, and
 * a game packet has the handshake bit clear.
 */
class FLOCK_API FFlockBridgeMessages
{
public:
	static constexpr uint8 Marker = 0xFA;
	static constexpr uint8 Version = 1;
	static constexpr uint8 Offer = 1;
	static constexpr uint8 Probe = 2;
	static constexpr uint8 Answer = 3;
	static constexpr uint8 KeepAlive = 4;
	static constexpr uint8 BackToTheRelay = 5;
	static constexpr uint8 NewConnection = 6;
	static constexpr int32 MostAddressesOffered = 4;
	/** The marker, "LNK", the version and the kind. */
	static constexpr int32 HeaderLength = 6;

	/** True for a message of the bridges' own, of any version: none reaches the engine. */
	static bool IsBridgeMessage(const uint8* Data, int32 Count);

	/** The kind of a message of this version; 0 for one of another version or too short to say. */
	static uint8 KindOf(const uint8* Data, int32 Count);

	/**
	 * An offer: the sender's key, the receiver's key as the sender holds it (zeros before it has heard one), and up to
	 * MostAddressesOffered addresses the sender may be reached at.
	 */
	static TArray<uint8> WriteOffer(const FFlockBridgeKey& SendersKey, const TOptional<FFlockBridgeKey>& ReceiversKey, const TArray<FFlockRelayPeer>& Addresses);

	/** Reads an offer; false for a short or malformed one. OutReceiversKey is unset when the sender had heard no key of the receiver's. */
	static bool TryReadOffer(const uint8* Data, int32 Count, FFlockBridgeKey& OutSendersKey, TOptional<FFlockBridgeKey>& OutReceiversKey,
		TArray<FFlockRelayPeer>& OutAddresses);

	/** A probe, answer, keep-alive or back-to-the-relay message: the receiver's key, then the sender's. */
	static TArray<uint8> WriteBetween(uint8 Kind, const FFlockBridgeKey& ReceiversKey, const FFlockBridgeKey& SendersKey);

	/** True when a probe, answer, keep-alive or back-to-the-relay message names both keys of this pair: ReceiversKey first. */
	static bool IsBetween(const uint8* Data, int32 Count, const FFlockBridgeKey& ReceiversKey, const FFlockBridgeKey& SendersKey);

	/** Word, through the relay, that the game behind a player's bridge started its connection numbered Number. */
	static TArray<uint8> WriteNewConnection(uint32 Number);

	/** Reads word of a new connection; false for anything else. */
	static bool TryReadNewConnection(const uint8* Data, int32 Count, uint32& OutNumber);

private:
	static void WriteHeader(TArray<uint8>& Message, uint8 Kind);
};
