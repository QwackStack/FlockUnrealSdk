// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockEventModels.h"
#include "Http/FlockError.h"
#include "Http/FlockErrorCode.h"

/**
 * Turns a failure into the next step the developer should take. The one owner of every fix the SDK hands out:
 * FFlockError::Make stamps ForFailure onto FFlockError::Hint, so no failure leaves the SDK without one. Keyed on
 * EFlockErrorCode first — never on message text, same rule as the error predicates — and on the kind of failure
 * when the code has no advice of its own.
 */
class FLOCK_API FFlockErrorHints
{
public:
	/** Next step for a coded error; empty when the code has no advice of its own (Unknown among them). */
	static FString For(EFlockErrorCode Code);

	/**
	 * Next step for any failure: its code's advice when it has one, otherwise the advice for its kind — a timeout,
	 * no connection, an answer that did not come from Flock's game API (no code), a code this SDK version does not
	 * know, a server failure. Never empty.
	 */
	static FString ForFailure(const FFlockError& Error);

	/**
	 * Next step for an auth failure. The credential disambiguates codes that mean different things per
	 * method — notably invalid_login_credentials, which means "register this device first" for a device
	 * login but "wrong password" for email. The HTTP layer can't know which, so the auth provider
	 * refines the context-free hint on the way out. Empty when the code has no advice of its own.
	 */
	static FString ForAuth(EFlockErrorCode Code, EFlockAuthMethod Method);

	/** Codes deliberately shipped without a hint of their own, so the coverage test can tell "waived" from "forgotten". */
	static bool IsWaived(EFlockErrorCode Code);

	// The fixes several failures the SDK finds itself share, kept here so each has one wording.

	/** A call that acts as the player, made with nobody signed in. */
	static FString SignInFirst();

	/** A call made before Flock initialized, or with an object outside the game's world as its context. */
	static FString InitializeFlockFirst();

	/** A 2xx answer that lacks what only Flock's game API would send. */
	static FString NotFlocksAnswer();

	/** A session call given no session. */
	static FString PassAHeldSession();

	/** A party call given no party. */
	static FString PassAHeldParty();

	/** A session that has ended, asked to do something. */
	static FString FindAnotherSession();

	/** A party that has ended, asked to do something. */
	static FString FindAnotherParty();

	/** A failed download of an asset's file, which comes from storage rather than the Flock server; 0 when storage was never reached. */
	static FString ForStorageDownload(int32 StatusCode);
};
