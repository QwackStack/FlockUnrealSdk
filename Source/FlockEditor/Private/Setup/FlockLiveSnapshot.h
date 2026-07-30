// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

/** The SDK's runtime state as the panel shows it during Play In Editor. */
struct FFlockLiveSnapshot
{
	/** False when no PIE session is running, or the game instance has no Flock subsystem. */
	bool bSdkPresent = false;

	bool bInitialized = false;
	FString InitializationError;

	bool bAuthenticated = false;
	bool bRestoringSession = false;
	FString PlayerId;

	bool bHasAnalyticsSession = false;
	FString AnalyticsSessionId;
	bool bAnalyticsConsent = false;

	int32 PendingCommandWrites = 0;
	bool bLikelyOffline = false;
};

/**
 * Where the live view gets its data.
 *
 * An interface with one implementation today, which is the point: a live HTTP request list is the
 * obvious next addition, and it plugs in here rather than reshaping the panel. Keeping the panel unaware
 * of *how* state is obtained is what makes that a drop-in.
 */
class IFlockLiveSnapshotSource
{
public:
	virtual ~IFlockLiveSnapshotSource() = default;

	virtual FFlockLiveSnapshot Capture() const = 0;
};

/** Reads the running PIE session's Flock subsystem. */
class FFlockPieSnapshotSource : public IFlockLiveSnapshotSource
{
public:
	virtual FFlockLiveSnapshot Capture() const override;
};
