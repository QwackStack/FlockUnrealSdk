// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

struct FFlockRelayTlsState;

/**
 * TLS for a relay reached over TLS, run over memory: what arrives from the network is handed in, what must go out is taken, and
 * the handshake and the records are the engine's OpenSSL's. The relay's certificate is checked against what the engine's SSL
 * module trusts (the platform's own certificates, as the engine's HTTP module does, the game's pinned keys included) and must
 * name the relay; nothing of the SDK's own decides it, so a network that inspects encrypted traffic is refused. Not
 * thread-safe: its owner holds one lock around every call.
 */
class FFlockRelayTls
{
public:
	/** True where this build of the game speaks TLS to the relay (the engine's SSL module and OpenSSL are linked). */
	static bool IsAvailable();

	/** Readies the engine's SSL module, which must first be reached on the game thread; false where TLS is unavailable. Game thread. */
	static bool ReadyOnTheGameThread();

	/** A certificate (PEM) trusted beside the platform's, for a test's own relay; empty outside tests. Any thread. */
	static void SetTrustedCertificateForTesting(const FString& Pem);

	/** A client for Host, whose certificate must name it; null with why in OutCause when TLS cannot be set up. */
	static TUniquePtr<FFlockRelayTls> Start(const FString& Host, FString& OutCause);

	~FFlockRelayTls();

	FFlockRelayTls(const FFlockRelayTls&) = delete;
	FFlockRelayTls& operator=(const FFlockRelayTls&) = delete;

	enum class EStep : uint8
	{
		/** The handshake is over: the relay's certificate was trusted and names it. */
		Done,
		/** The handshake waits for the relay's next bytes (and has bytes to send first, when HasBytesForTheNetwork says so). */
		Waiting,
		Failed,
	};

	/** Takes the handshake as far as what arrived lets it; Failed with why in OutCause. */
	EStep Handshake(FString& OutCause);

	/** Hands over bytes that arrived from the relay. */
	void PutArrived(const uint8* Bytes, int32 Count);

	/** What arrived, decrypted, into Into (up to Size bytes): how many; 0 while a whole record has not arrived; -1 when TLS ended or broke, with why in OutCause. */
	int32 ReadPlain(uint8* Into, int32 Size, FString& OutCause);

	/** Encrypts Count bytes for the relay; false with why in OutCause when TLS refused them. */
	bool WritePlain(const uint8* Bytes, int32 Count, FString& OutCause);

	/** True while TLS has bytes for the network (records, or its own answers to what the relay sent). */
	bool HasBytesForTheNetwork() const;

	/** Moves every byte TLS has for the network onto the end of Out. */
	void TakeForTheNetwork(TArray<uint8>& Out);

private:
	explicit FFlockRelayTls(TUniquePtr<FFlockRelayTlsState> InState);

	TUniquePtr<FFlockRelayTlsState> State;
};
