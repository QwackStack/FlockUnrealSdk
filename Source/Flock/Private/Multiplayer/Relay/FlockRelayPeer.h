// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/** An IPv4 address and port on the relay: a player's relay address, or where a packet came from. A value, so handing packets over allocates nothing. */
struct FLOCK_API FFlockRelayPeer
{
	/** The four address bytes, the first one highest. */
	uint32 Address = 0;
	int32 Port = 0;

	FFlockRelayPeer() = default;
	FFlockRelayPeer(uint32 InAddress, int32 InPort) : Address(InAddress), Port(InPort) {}

	bool IsEmpty() const { return Address == 0 && Port == 0; }

	bool operator==(const FFlockRelayPeer& Other) const { return Address == Other.Address && Port == Other.Port; }
	bool operator!=(const FFlockRelayPeer& Other) const { return !(*this == Other); }

	friend uint32 GetTypeHash(const FFlockRelayPeer& Peer) { return HashCombine(::GetTypeHash(Peer.Address), ::GetTypeHash(Peer.Port)); }

	/** The address as "a.b.c.d", without the port. */
	FString Ip() const { return IpOf(Address); }

	FString ToString() const { return FString::Printf(TEXT("%s:%d"), *Ip(), Port); }

	static FString IpOf(uint32 Address)
	{
		return FString::Printf(TEXT("%u.%u.%u.%u"), (Address >> 24) & 0xFF, (Address >> 16) & 0xFF, (Address >> 8) & 0xFF, Address & 0xFF);
	}

	/** Reads "a.b.c.d:port", digits only; false for anything else (a sign, a space, a part over 255, a port out of range). */
	static bool TryParse(const FString& Text, FFlockRelayPeer& OutPeer);

	/**
	 * The one reader of a connection of mode "relay" (letter case counts): the relay address it names and, on the host's, the
	 * relay server ("name:port", empty when absent). False for another mode or no usable relay address. Reads the host's
	 * connection for the session and a player's for its seat alike.
	 */
	static bool TryReadPublished(const FJsonObject& Connection, FFlockRelayPeer& OutAddress, FString& OutServer);

	/**
	 * True when a connection of mode "relay" says the device behind it reads version 1 of the relay bridges' own messages
	 * (FlockRelayConnectionValues::BridgeMessages: the highest version read, every earlier one included, so 1 or more); a device
	 * that does not say so would hand them to the engine, which closes a connection they reach, so none is sent to it.
	 */
	static bool ReadsBridgeMessages(const FJsonObject& Connection);
};
