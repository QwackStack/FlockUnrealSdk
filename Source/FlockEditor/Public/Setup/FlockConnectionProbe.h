// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Setup/FlockSetupStatus.h"

struct FFlockShop;

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

	/** Whether the game's shops sell an item for real money; unset when the probe could not read every shop. */
	TOptional<bool> SellsRealMoney;
};

DECLARE_DELEGATE_OneParam(FFlockProbeComplete, const FFlockProbeResult&);

/**
 * The editor's "is this actually configured correctly" check.
 *
 * Sequential requests, because one cannot answer everything:
 *
 *   1. `game_version/by-name/{name}` with only the API key. Exercises the URL, the key, and the version
 *      name in one shot, and yields the version ID. This is the call the resolve step already makes.
 *   2. `game` with the key plus that version ID, whose record carries the game's real name.
 *   3. `shop`, page by page, to learn whether the game sells anything for real money (the engine's own
 *      purchase nodes are warned of only then). It stops at the first real-money item.
 *
 * Steps 2 and 3 need step 1's output (the `X-Game-Version-ID` header), so the order is forced, and a
 * failure in step 1 ends the probe — there is nothing useful to ask afterwards. A failure in step 2 or 3
 * leaves the probe Ok, without the answer that step would have given.
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

	/** True when one of these shops sells an item for real money. Pure. */
	static bool SellsRealMoney(const TArray<FFlockShop>& Shops);

	/** Runs every step and reports once. Never throws; a failure is a state, not an error path. */
	static void Run(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion,
		FFlockProbeComplete OnComplete);
};
