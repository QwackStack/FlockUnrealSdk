// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockEventModels.h"
#include "Http/FlockErrorCode.h"

/**
 * Turns a coded server error into the next step the developer should take. Keyed on EFlockErrorCode
 * only — never on message text, same rule as the error predicates. Stamped onto FFlockError::Hint by
 * FFlockError::Make, so every coded failure carries its own remedy.
 */
class FLOCK_API FFlockErrorHints
{
public:
	/** Next step for a coded error; empty when the SDK has nothing to add beyond the server's reason. */
	static FString For(EFlockErrorCode Code);

	/**
	 * Next step for an auth failure. The credential disambiguates codes that mean different things per
	 * method — notably invalid_login_credentials, which means "register this device first" for a device
	 * login but "wrong password" for email. The HTTP layer can't know which, so the auth provider
	 * refines the context-free hint on the way out.
	 */
	static FString ForAuth(EFlockErrorCode Code, EFlockAuthMethod Method);

	/** Codes deliberately shipped without a hint, so the coverage test can tell "waived" from "forgotten". */
	static bool IsWaived(EFlockErrorCode Code);
};
