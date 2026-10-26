// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * A server's first address of one family, looked up on the engine's own threads and read from any thread once it says it is
 * done: the STUN and relay servers Flock lists, IPv4 (through the engine's lookup), and a relay server's IPv6 address for a way
 * in over IPv6, a network with IPv6 alone (through the platform's own resolver: the engine's drops IPv6 answers while its
 * net.DisableIPv6 is 1, its default). An address as written is answered at once, as it is when it is of the family asked, else with nothing; an IPv6
 * address written as the host, or text with a space in it, at once with nothing. A lookup takes no deadline of its own and can
 * hang for as long as the network's resolver does, so whoever waits on it bounds the wait.
 */
class FFlockNameLookup
{
public:
	/** Stands in for the system's lookup: hands Answer every address of the host (of either family; none for no answer), on any thread, or never. */
	using FLookUp = TFunction<void(const FString& Host, TFunction<void(const TArray<FString>& Addresses)> Answer)>;

	/** Starts looking Host up for its first IPv4 address (IPv6 when bIpv6), through LookUpForTesting when one is given. */
	static TSharedRef<FFlockNameLookup, ESPMode::ThreadSafe> Start(const FString& Host, const FLookUp& LookUpForTesting, bool bIpv6 = false);

	/** True once answered; GetAddress is then the address, or empty when the name gave none of the family asked. */
	bool IsDone() const;
	FString GetAddress() const;

private:
	explicit FFlockNameLookup(bool bInIpv6) : bIpv6(bInIpv6) {}

	/** The first of Found of this lookup's family. */
	void AnswerWithTheFirstOf(const TArray<FString>& Found);
	void Answer(const FString& Found);

	const bool bIpv6;
	mutable FCriticalSection Lock;
	bool bDone = false;
	FString Address;
};
