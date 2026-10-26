// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

class FInternetAddr;
class FSocket;

/**
 * Opens the sockets that reach servers on the internet. UDP ones are dual-stack (IPv6 carrying IPv4 too) where the platform
 * allows, plain IPv4 where it does not: on a phone on mobile data, IPv4 sockets never heard a STUN server's answers while
 * dual-stack ones always did (measured on one carrier). A TCP one, for a relay reached over TCP or TLS, is of its server's
 * address family. Addresses stay IPv4 everywhere but the way to a relay server over IPv6: one is sent to in its IPv6 form only
 * on a dual-stack socket. Any thread: the relay opens its sockets on a thread of its own.
 */
class FFlockSockets
{
public:
	/**
	 * True where this platform's sockets reach IPv6 addresses: the engine carries IPv6 there (it does not on Android, whose
	 * engine sockets hold IPv4 addresses alone) and an IPv6 socket can be made.
	 */
	static bool CarriesIpv6();

	/** True for text that is an IPv6 address (it holds a colon; a host name or an IPv4 address never does). */
	static bool IsIpv6Address(const FString& Address) { return Address.Contains(TEXT(":")); }

	/**
	 * A non-blocking TCP socket of Address's family (IPv4 or IPv6 text), its packets sent at once rather than joined, with its
	 * connection to Address:Port started; null when none can be made or the connection was refused at once, with why in OutCause.
	 */
	static FSocket* StartStreamTo(const FString& Address, int32 Port, const TCHAR* Description, FString& OutCause);

	/** A non-blocking UDP socket bound to any address and a port of the system's choosing, dual-stack where possible; null when none can be made. */
	static FSocket* Open(const TCHAR* Description);

	/** A plain IPv4 socket, as Open makes where IPv6 is refused; null when none can be made. */
	static FSocket* OpenIpv4(const TCHAR* Description);

	/** The address of this device's loopback the relay bridge's sockets use. */
	static constexpr const TCHAR* LoopbackIpv4 = TEXT("127.0.0.1");
	static constexpr int32 LoopbackBufferBytes = 256 * 1024;

	/**
	 * A non-blocking IPv4 socket bound to 127.0.0.1 on a port of the system's choosing, through which the relay bridge hands
	 * packets to and from the engine's own net driver on this device; null when none can be made. Never dual-stack: nothing on
	 * the loopback crosses a network.
	 */
	static FSocket* OpenOnLoopback(const TCHAR* Description);

	/** 127.0.0.1 on Port, as a socket from OpenOnLoopback sends to it. */
	static TSharedRef<FInternetAddr> OnLoopback(int32 Port);

	/** True for an address on this device's loopback (127.x.y.z), in IPv4 or in its IPv6 form. */
	static bool IsLoopback(const FInternetAddr& Address);

	/**
	 * A socket from Open connected to Address:Port, so it reads that server's packets alone and sends without an address of its
	 * own; to an IPv4 address, a dual-stack one that cannot connect to it is replaced by a plain IPv4 one. An IPv6 address (a
	 * relay server reached over IPv6) needs a dual-stack socket. Null when none can be made or connected.
	 */
	static FSocket* OpenConnectedTo(const FString& Address, int32 Port, const TCHAR* Description);

	static void Close(FSocket* Socket);

	/** True for a socket that carries IPv4 through IPv6. */
	static bool IsDualStack(const FSocket& Socket);

	/** An address and port as Socket sends to them: an IPv4 one in its IPv6 form (::ffff:a.b.c.d) on a dual-stack socket. */
	static TSharedRef<FInternetAddr> Toward(const FSocket& Socket, const FString& Address, int32 Port);

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
