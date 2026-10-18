// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * A server's first IPv4 address, looked up on the engine's own threads and read from any thread once it says it is done: the
 * STUN and relay servers Flock lists. An address as written is answered at once, as it is; an IPv6 address, or text with a
 * space in it, at once with nothing, since every request goes over UDP to IPv4. A lookup takes no deadline of its own and can
 * hang for as long as the network's resolver does, so whoever waits on it bounds the wait.
 */
class FFlockNameLookup
{
public:
	/** Stands in for the system's lookup: hands Answer the host's first IPv4 address (empty for none), on any thread, or never. */
	using FLookUp = TFunction<void(const FString& Host, TFunction<void(const FString& Ipv4)> Answer)>;

	/** Starts looking Host up, through LookUpForTesting when one is given. */
	static TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> Start(const FString& Host, const FLookUp& LookUpForTesting);

	/** True once answered; GetIpv4 is then the address, or empty when the name gave none. */
	bool IsDone() const;
	FString GetIpv4() const;

private:
	void Answer(const FString& Found);

	mutable FCriticalSection Lock;
	bool bDone = false;
	FString Ipv4;
};
