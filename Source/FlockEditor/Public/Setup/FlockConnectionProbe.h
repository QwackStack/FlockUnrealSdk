// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Setup/FlockSetupStatus.h"

/** What a Test Connection learned. Feeds straight into FFlockSetupInput. */
struct FLOCKEDITOR_API FFlockProbeResult
{
	EFlockProbeState State = EFlockProbeState::NotRun;

	/** Server wording for a failure that could not be pinned to one credential. */
	FString Message;

	/** Version ID the probe resolved, when it got that far. */
	FString GameVersionId;

	/** Game name the server reports for this API key; empty when the probe never reached the game record. */
	FString ServerGameName;
};

DECLARE_DELEGATE_OneParam(FFlockProbeComplete, const FFlockProbeResult&);

/**
 * The editor's "is this actually configured correctly" check.
 *
 * Two sequential requests, because one cannot answer everything:
 *
 *   1. `game_version/by-name/{name}` with only the API key. Exercises the URL, the key, and the version
 *      name in one shot, and yields the version ID. This is the call the resolve step already makes.
 *   2. `game` with the key plus that version ID, whose record carries the game's real name.
 *
 * Step 2 needs step 1's output (the `X-Game-Version-ID` header), so the order is forced, and a failure
 * in step 1 ends the probe — there is nothing useful to ask afterwards.
 */
class FLOCKEDITOR_API FFlockConnectionProbe
{
public:
	/**
	 * Maps a typed failure onto a probe state. Pure, so the mapping is testable without a backend.
	 *
	 * Timeout deliberately does not report Unreachable: "check your API URL" would send a developer to
	 * change a setting that is very likely correct. It reports Failed with the server's own wording.
	 */
	static EFlockProbeState Classify(const FFlockError& Error);

	/** Runs both steps and reports once. Never throws; a failure is a state, not an error path. */
	static void Run(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion,
		FFlockProbeComplete OnComplete);
};
