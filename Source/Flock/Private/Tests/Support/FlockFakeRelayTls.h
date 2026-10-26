// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

struct FFlockFakeRelayTlsState;

/**
 * The fake relay's side of TLS, over memory, with the test certificate (FlockFakeRelayCertificate): its own use of the engine's
 * OpenSSL, so it checks the SDK's client rather than sharing its code. One thread. Include only from guarded test files.
 */
class FFlockFakeRelayTls
{
public:
	/** The relay's side of one connection; null where this build has no TLS. */
	static TUniquePtr<FFlockFakeRelayTls> Start();

	~FFlockFakeRelayTls();

	FFlockFakeRelayTls(const FFlockFakeRelayTls&) = delete;
	FFlockFakeRelayTls& operator=(const FFlockFakeRelayTls&) = delete;

	/** Takes what the client sent: the handshake moves on, and what it decrypts is put on the end of OutPlain; false once TLS broke. */
	bool Arrived(const uint8* Bytes, int32 Count, TArray<uint8>& OutPlain);

	/** Encrypts Count bytes for the client; false before the handshake is over or once TLS broke. */
	bool Write(const uint8* Bytes, int32 Count);

	/** Moves every byte TLS has for the client onto the end of Out. */
	void TakeForTheNetwork(TArray<uint8>& Out);

private:
	explicit FFlockFakeRelayTls(TUniquePtr<FFlockFakeRelayTlsState> InState);

	TUniquePtr<FFlockFakeRelayTlsState> State;
};

#endif
