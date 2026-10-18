// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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

	/** `<ProjectSavedDir>/Flock/auth.dat`: the sign-in every game shared before 1.33.0. */
	static FString DefaultPath();

	/** `<ProjectSavedDir>/Flock`, where the saved sign-ins are kept. */
	static FString DefaultFolder();

	/** One game's sign-in file in Folder, `auth-<game key>.dat`; `auth.dat` for the one every game shares (an empty key). */
	static FString PathOf(const FString& Folder, const FString& GameKey);

	virtual void Save(const FFlockStoredTokens& Tokens) override;
	virtual bool Load(FFlockStoredTokens& OutTokens) override;
	virtual void Clear() override;

private:
	FString FilePath;
	FString KeyContext;
};
