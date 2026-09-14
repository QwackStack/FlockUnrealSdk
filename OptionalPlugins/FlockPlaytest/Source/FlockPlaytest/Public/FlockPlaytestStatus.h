// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

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

	/**
	 * The settings are complete and the Flock SDK is initialized. It does not mean a player is signed in or a
	 * Flock session exists: the Flock SDK announces that it is initialized before it restores a session.
	 */
	Ready,

	/** The playtest subsystem has shut down with its game instance. Nothing runs after this. */
	Stopped,
};

/** The facts the status is decided from, gathered by the caller so the decision reads no engine state. */
struct FFlockPlaytestStatusInputs
{
	bool bPlaytestingEnabled = false;
	FString ProtokiteApiUrl;
	bool bFlockInitialized = false;
};

/**
 * Decides the playtest status from its inputs.
 *
 * The order is the switch, then the URL, then the Flock SDK. A build with playtesting turned off says
 * nothing about its URL, and a URL mistake is reported straight away rather than hidden until the Flock
 * SDK initializes.
 */
FLOCKPLAYTEST_API EFlockPlaytestStatus DecidePlaytestStatus(const FFlockPlaytestStatusInputs& Inputs);

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
