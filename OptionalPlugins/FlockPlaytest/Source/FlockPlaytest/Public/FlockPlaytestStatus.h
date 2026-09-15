// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockPlaytestConfig.h"
#include "Http/FlockResult.h"

/** Whether this build may do playtest work right now, and if not, why. */
enum class EFlockPlaytestStatus : uint8
{
	/** Enable Playtesting is off. Nothing else is looked at. */
	TurnedOff,

	/** Enable Playtesting is on, but Protokite API URL is empty. */
	ProtokiteApiUrlMissing,

	/** Protokite API URL is set but cannot be used: no http:// or https://, no host, or whitespace in it. */
	ProtokiteApiUrlUnusable,

	/** The settings are complete, and the Flock SDK is not initialized (not yet, or shut down). */
	WaitingForFlock,

	/** The settings are complete, the Flock SDK is initialized, and this build's playtest is being fetched. */
	FetchingPlaytestConfig,

	/** Protokite has no playtest linked to this build's Game Version ID (HTTP 404). */
	PlaytestNotLinked,

	/** Protokite refused the Flock API key (HTTP 401), or the request reached it without one (HTTP 422). */
	ProtokiteRefusedApiKey,

	/**
	 * The playtest could not be fetched: Protokite or the network kept failing through the retries, or the
	 * answer could not be read. Fetched again when the next Flock session starts, or when the Flock SDK
	 * initializes again.
	 */
	PlaytestConfigUnavailable,

	/** Protokite answered with the playtest of a different Game Version ID than the one this build sent. */
	PlaytestConfigForAnotherVersion,

	/**
	 * Protokite refused this launch's session because the playtest has closed and takes no more sessions (HTTP 400).
	 * Playtest work stays off until the game is launched again, even when the Flock SDK initializes again.
	 */
	PlaytestNoLongerCollecting,

	/**
	 * Playtest work may run: the settings are complete, the Flock SDK is initialized and this build's playtest
	 * config is loaded. It does not mean a player is signed in or a Flock session exists: the Flock SDK
	 * announces that it is initialized before it restores a session.
	 */
	Ready,

	/** The playtest subsystem has shut down with its game instance. Nothing runs after this. */
	Stopped,
};

/** How far fetching the playtest config has got for the current Flock initialization. */
enum class EFlockPlaytestConfigState : uint8
{
	NotFetched,
	Fetching,
	Loaded,
	PlaytestNotLinked,
	ApiKeyRefused,
	Unavailable,
	ForAnotherVersion,
};

/** The facts the status is decided from, gathered by the caller so the decision reads no engine state. */
struct FFlockPlaytestStatusInputs
{
	bool bPlaytestingEnabled = false;
	FString ProtokiteApiUrl;
	bool bFlockInitialized = false;
	EFlockPlaytestConfigState ConfigState = EFlockPlaytestConfigState::NotFetched;

	/** Protokite refused this launch's session because the playtest has closed. */
	bool bPlaytestNoLongerCollecting = false;
};

/**
 * Decides the playtest status from its inputs.
 *
 * The order is the switch, then the URL, then a closed playtest, then the Flock SDK, then the playtest config. A
 * build with playtesting turned off says nothing about its URL, a URL mistake is reported straight away rather than
 * hidden until the Flock SDK initializes, and a closed playtest stays closed whatever the Flock SDK does next.
 */
FLOCKPLAYTEST_API EFlockPlaytestStatus DecidePlaytestStatus(const FFlockPlaytestStatusInputs& Inputs);

/**
 * What a finished playtest-config fetch means. Protokite's refusals carry no code, so they are told apart by
 * HTTP status: 404 is no linked playtest, 401 a refused key, 422 a request without one. Anything else that
 * failed is unavailable, including an answer that could not be read and a 403, which Protokite does not send on
 * this route. A config naming a different Game Version ID than the one sent, even only by letter case, is not
 * loaded.
 */
FLOCKPLAYTEST_API EFlockPlaytestConfigState DecidePlaytestConfigState(const TFlockResult<FFlockPlaytestConfig>& Result,
	const FString& SentGameVersionId);

/**
 * True when Url starts with http:// or https:// (in any letter case), names a host, and contains no
 * whitespace anywhere.
 *
 * Never trims. A space or line break pasted into the setting is refused, so the mistake is fixed where it
 * was made instead of being quietly worked around at runtime.
 */
FLOCKPLAYTEST_API bool IsUsableProtokiteApiUrl(const FString& Url);

/** One sentence describing the status, naming the setting to change when there is one. */
FLOCKPLAYTEST_API FString DescribePlaytestStatus(EFlockPlaytestStatus Status);
