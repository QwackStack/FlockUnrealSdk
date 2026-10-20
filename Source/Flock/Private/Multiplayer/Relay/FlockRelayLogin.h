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

/** One relay server over UDP and the login Flock minted for it, read from Flock's answer of servers to connect through. */
struct FFlockRelayLogin
{
	static constexpr int32 DefaultPort = 3478;

	FString Host;
	int32 Port = DefaultPort;
	FString Username;
	FString Password;

	/** The server as "name:port", which names it to the players who reserve on the host's server. */
	FString ToString() const { return FString::Printf(TEXT("%s:%d"), *Host, Port); }

	/** Every relay entry's UDP servers, in Flock's order (more than one while Flock moves between relays); STUN entries and entries without a login are left out. */
	static TArray<FFlockRelayLogin> InAnswer(const FFlockRelayLoginList& Answer);

	/** A "turn:host[:port][?transport=udp]" address; false for TCP, TLS ("turns:"), an IPv6 host or anything unreadable, since this client speaks UDP over IPv4. */
	static bool TryReadUdpServer(const FString& Url, FString& OutHost, int32& OutPort);
};

/** A STUN server Flock lists: a name to look up and its port. */
struct FFlockRelayStunServer
{
	FString Host;
	int32 Port = FFlockRelayLogin::DefaultPort;

	/** The STUN servers in Flock's answer, in its order; "stun:host[:port]" entries only. */
	static TArray<FFlockRelayStunServer> InAnswer(const FFlockRelayLoginList& Answer);
};
