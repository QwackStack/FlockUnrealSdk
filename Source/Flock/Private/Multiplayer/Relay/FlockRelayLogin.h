// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * The relay route's answer for a seat (measured 2026-10-17): how long the logins last, each server entry ("urls", and a
 * "username" and "credential" for a relay, both null for STUN), and whether the studio's relay is paused.
 */
struct FFlockRelayLoginList
{
	struct FEntry
	{
		TArray<FString> Urls;
		FString Username;
		FString Credential;
	};

	int32 TtlSeconds = 0;
	TArray<FEntry> Entries;
	bool bRelayPaused = false;

	/** Reads the answer; one with no list of servers is not the server's answer, and fails the read. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockRelayLoginList& OutStruct, FString& OutError);
};

/** How a relay connection reaches its server: UDP, or TCP or TLS for a network that blocks UDP to the relay. */
enum class EFlockRelayTransport : uint8
{
	Udp,
	Tcp,
	Tls,
};

/** One way in to a relay server (its address and transport) and the login Flock minted for it, read from Flock's answer of servers to connect through. */
struct FFlockRelayLogin
{
	static constexpr int32 DefaultPort = 3478;
	static constexpr int32 DefaultTlsPort = 5349;

	FString Host;
	int32 Port = DefaultPort;
	FString Username;
	FString Password;
	EFlockRelayTransport Transport = EFlockRelayTransport::Udp;
	/** True when the way reaches the server at its IPv6 address, for a network with IPv6 alone; the relay still gives an IPv4 relay address. */
	bool bOverIpv6 = false;

	/** The server as "name:port", which names it to the players who reserve on the host's server. */
	FString ToString() const { return FString::Printf(TEXT("%s:%d"), *Host, Port); }

	/** The way in as a message names it, such as "turn.example.com:5349 over TLS" or "turn.example.com:3478 over UDP on IPv6". */
	FString GetWay() const;

	/**
	 * Whether this way leads to Server ("name:port", as a host publishes the server it uses): the same name, over any port and
	 * transport, since every way in to one server reaches the same relay.
	 */
	bool IsOn(const FString& Server) const { return SameServer(ToString(), Server); }

	/** Whether two "name:port" texts name one relay server: the same name, whatever the port. */
	static bool SameServer(const FString& One, const FString& Other);

	/**
	 * Every relay entry's ways in, in Flock's order (more than one server while Flock moves between relays); STUN entries, entries
	 * without a login and ways this client cannot use are left out.
	 */
	static TArray<FFlockRelayLogin> InAnswer(const FFlockRelayLoginList& Answer);

	/**
	 * Logins as they are tried: UDP over IPv4, UDP over IPv6, then TCP and TLS over IPv4, then over IPv6, each in the order given; a
	 * server given as an IP address has no IPv6 way. UDP never holds a packet back for a lost one, so it comes first on either
	 * family: a phone hotspot dropped UDP over IPv4 alone and carried it over IPv6 (measured 2026-10-25).
	 */
	static TArray<FFlockRelayLogin> InTheOrderTried(const TArray<FFlockRelayLogin>& Logins);

	/**
	 * A "turn:host[:port][?transport=udp|tcp]" or "turns:host[:port][?transport=tcp]" address (TLS, port 5349 when none is given);
	 * false for DTLS ("turns:" over UDP), an IPv6 host or anything unreadable.
	 */
	static bool TryReadServer(const FString& Url, FString& OutHost, int32& OutPort, EFlockRelayTransport& OutTransport);
};

/** A STUN server Flock lists: a name to look up and its port. */
struct FFlockRelayStunServer
{
	FString Host;
	int32 Port = FFlockRelayLogin::DefaultPort;

	/** The STUN servers in Flock's answer, in its order; "stun:host[:port]" entries only. */
	static TArray<FFlockRelayStunServer> InAnswer(const FFlockRelayLoginList& Answer);
};
