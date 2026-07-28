// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockEventModels.h"

/** Tokens persisted between launches, plus how the session was established (drives method-gated flows after a restore). */
struct FFlockStoredTokens
{
	FString AccessToken;
	FString RefreshToken;
	/** Unset when the stored file predates method persistence or the value didn't parse. */
	TOptional<EFlockAuthMethod> AuthMethod;
};

/**
 * Persistence seam for auth tokens between app launches. The SDK default is FFlockFileTokenStore
 * (encrypted file under the project's Saved dir); implement this to plug in platform-secure or
 * custom storage. Implementations must never throw and should treat I/O failures as non-fatal.
 */
class FLOCK_API IFlockTokenStore
{
public:
	virtual ~IFlockTokenStore() = default;

	virtual void Save(const FFlockStoredTokens& Tokens) = 0;

	/** Returns false when there is no usable stored session (missing, corrupt, or empty access token). */
	virtual bool Load(FFlockStoredTokens& OutTokens) = 0;

	virtual void Clear() = 0;
};
