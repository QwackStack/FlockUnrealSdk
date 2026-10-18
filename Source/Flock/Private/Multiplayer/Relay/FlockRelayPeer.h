// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

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
};
