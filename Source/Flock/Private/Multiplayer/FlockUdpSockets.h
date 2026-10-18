// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

class FInternetAddr;
class FSocket;

/**
 * Opens the UDP sockets that reach servers on the internet: dual-stack (IPv6 carrying IPv4 too) where the platform allows,
 * plain IPv4 where it does not. On a phone on mobile data, IPv4 sockets never heard a STUN server's answers while dual-stack
 * ones always did (measured on one carrier). Addresses stay IPv4 everywhere else: one is sent to in its IPv6 form only on a
 * dual-stack socket. Any thread: the relay opens its sockets on a thread of its own.
 */
class FFlockUdpSockets
{
public:
	/** A non-blocking UDP socket bound to any address and a port of the system's choosing, dual-stack where possible; null when none can be made. */
	static FSocket* Open(const TCHAR* Description);

	/** A plain IPv4 socket, as Open makes where IPv6 is refused; null when none can be made. */
	static FSocket* OpenIpv4(const TCHAR* Description);

	/**
	 * A socket from Open connected to Ipv4Address:Port, so it reads that server's packets alone and sends without an address
	 * of its own; a dual-stack one that cannot connect to it is replaced by a plain IPv4 one. Null when none can be made or
	 * connected.
	 */
	static FSocket* OpenConnectedTo(const FString& Ipv4Address, int32 Port, const TCHAR* Description);

	static void Close(FSocket* Socket);

	/** True for a socket that carries IPv4 through IPv6. */
	static bool IsDualStack(const FSocket& Socket);

	/** An IPv4 address and port as Socket sends to them: in their IPv6 form (::ffff:a.b.c.d) on a dual-stack socket. */
	static TSharedRef<FInternetAddr> Toward(const FSocket& Socket, const FString& Ipv4Address, int32 Port);

	/** The "any sender" a receive on Socket fills in. */
	static TSharedRef<FInternetAddr> AnySender(const FSocket& Socket);

	/** The port Socket sends from, which the server sees (through any router between). */
	static int32 LocalPortOf(FSocket& Socket);

	/** Makes a dual-stack socket fail to be made, as on a platform without IPv6. */
	static std::atomic<bool> bDualStackRefusedForTesting;

	/** Makes a dual-stack socket's send to an IPv4 address fail, as on a platform that makes IPv6 sockets but carries no IPv4 through them. */
	static std::atomic<bool> bDualStackSendRefusedForTesting;

	/** Makes a dual-stack socket's connect to an IPv4 address fail, the same platform seen from a connected socket. */
	static std::atomic<bool> bDualStackConnectRefusedForTesting;

	/** Clears the stand-ins and a refusal remembered from one. */
	static void ResetForTesting();

	static bool RemembersARefusalForTesting() { return bDualStackRefused; }

private:
	/** The platform refused a dual-stack socket once: it will again, so later sockets go straight to IPv4 for the rest of the launch. */
	static std::atomic<bool> bDualStackRefused;
};
