// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

class UFlockConfig;

/**
 * Runtime analytics knobs, lifted off UFlockConfig so the analytics core stays decoupled from the
 * UDeveloperSettings object and tests can construct one directly.
 *
 * The defaults below are the shipping defaults; FromSettings() overwrites every member.
 */
struct FLOCK_API FFlockAnalyticsConfig
{
	/** Master switch. When off every entry point is a no-op — nothing touches disk or the network. */
	bool bEnabled = true;

	/** GDPR-style opt-in. When on, nothing is collected (not even locally) until consent is granted. */
	bool bRequireExplicitConsent = false;

	bool bAutoStartSession = true;
	bool bAutoEndSessionOnQuit = true;

	/** Time backgrounded before the next foreground starts a fresh session instead of resuming. */
	float SessionTimeoutSeconds = 30.f;

	/** 0 disables the heartbeat entirely. */
	float HeartbeatIntervalSeconds = 60.f;

	/** Sessions shorter than this end with is_bounce set. */
	float BounceThresholdSeconds = 10.f;

	bool bPersistSessionOnDisk = true;
	bool bTrackFps = true;
	float FpsSampleIntervalSeconds = 1.f;

	bool bCacheFailedEvents = true;

	/** Cap on spooled entries. Oldest are dropped first once it is hit. */
	int32 MaxCachedEvents = 1000;

	/**
	 * Cap on spooled session ends, kept separate because ends are rare next to log events — if this
	 * many have gone undelivered, the oldest are of no further interest. Not surfaced in the project
	 * settings: it is a safety limit, not a tuning knob.
	 */
	int32 MaxCachedSessionEnds = 50;

	/** How many spooled entries go out per flush batch. */
	int32 CacheFlushBatchSize = 50;

	/** 0 disables the periodic flush; an explicit Flush() still works. */
	float EventBufferFlushIntervalSeconds = 10.f;

	/**
	 * Automatic capture of engine errors, fatals and Blueprint script exceptions as `exception` log events.
	 * Governed by these settings alone. A manual LogException still records with this off.
	 */
	bool bCaptureExceptions = true;

	/**
	 * Log categories whose errors are never reported as exceptions, on top of the SDK's own, which are always
	 * excluded. The defaults are the automation framework's: it reports a failing test as an Error line, and
	 * that is not a fault in the game.
	 */
	TArray<FString> ExcludedExceptionCategories = { TEXT("LogAutomationController"), TEXT("LogAutomationCommandLine") };

	/**
	 * Repeats of the same captured exception inside this window are counted rather than reported, then sent as
	 * one summary carrying the count. 0 reports every occurrence.
	 */
	float ExceptionRepeatWindowSeconds = 60.f;

	/** Builds the runtime config from the project's UFlockConfig settings. */
	static FFlockAnalyticsConfig FromSettings(const UFlockConfig& Settings);
};
