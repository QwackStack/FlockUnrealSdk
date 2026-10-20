// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/**
 * Names one game as the server knows it: the backend's address and the game's API key, the only two things every request carries
 * that tell games apart (the Game Name setting is free text, and two backends can hold games of one name). What a game keeps on the
 * device between launches (its saved sign-in, its analytics files) is kept under this name, so one game never restores or sends
 * another's. The first 16 hex characters of a SHA-1, so the API key itself is never written to disk.
 */
struct FFlockGameKey
{
	/** The key for this backend and API key: the address without surrounding spaces or trailing slashes, in any letter case, and the API key without surrounding spaces. */
	static FString Of(const FString& ApiUrl, const FString& ApiKey);

	/** A name kept for one game: Name followed by the game's key; Name alone for what every game shares (an empty key). */
	static FString Named(const FString& Name, const FString& GameKey);
};
