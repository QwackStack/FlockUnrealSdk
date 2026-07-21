// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockTokenStore.h"

/**
 * Default IFlockTokenStore: an AES-256-encrypted file under the project's Saved dir, keyed from
 * the OS login id + a caller context (the game id), so the file is bound to this machine/user and
 * install rather than protected by an on-disk key.
 *
 * Threat model: defeats casual file copying and inspection — not code running as the same user.
 * Platform keychain/keystore implementations slot in behind IFlockTokenStore when they land.
 *
 * All failures are non-fatal: Load answers "no usable session" (deleting a corrupt file),
 * Save/Clear log-and-continue.
 */
class FLOCK_API FFlockFileTokenStore : public IFlockTokenStore
{
public:
	/** KeyContext feeds key derivation (pass the game id); empty FilePath uses DefaultPath(). */
	explicit FFlockFileTokenStore(const FString& InFilePath, const FString& InKeyContext);

	/** `<ProjectSavedDir>/Flock/auth.dat`. */
	static FString DefaultPath();

	virtual void Save(const FFlockStoredTokens& Tokens) override;
	virtual bool Load(FFlockStoredTokens& OutTokens) override;
	virtual void Clear() override;

private:
	FString FilePath;
	FString KeyContext;
};
