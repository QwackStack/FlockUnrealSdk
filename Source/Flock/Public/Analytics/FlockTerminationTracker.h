// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * The tombstone left behind by a run, read on the next launch to work out how the last one died.
 *
 * Plain struct rather than a USTRUCT: it is an internal on-disk record, not a wire model. Its
 * fields are lifted into the app_termination event's data by the provider.
 */
struct FLOCK_API FFlockTerminationMarker
{
	/** `foreground` or `background` at the moment the app was last seen alive. */
	FString LastState;
	FString SessionId;
	FString ServerSessionId;
	FString PlayerId;
	/** Best estimate of the time of death: the last heartbeat or state change that got persisted. */
	FDateTime LastAliveUtc = FDateTime::MinValue();
	/** Unhandled exceptions seen during that run. Context, not proof of a crash. */
	int32 ExceptionCount = 0;
	FString AppVersion;
	FString SdkVersion;

	/** A marker with no session id tells us nothing and is treated as absent. */
	bool IsValid() const { return !SessionId.IsEmpty(); }
};

/**
 * Next-launch dirty-exit detection: keeps a marker alive while the game runs, and classifies any
 * marker that survived into the following launch.
 *
 * The logic is "a clean shutdown deletes the marker". Anything that skips the quit path — a crash,
 * a force-kill, a foreground OOM, power loss — leaves it behind, and finding one on boot is the
 * evidence that the previous run died. There is no other reliable signal for this.
 *
 * Classification is lifecycle-only: died backgrounded means the OS evicted it (or the player
 * swipe-closed, which backgrounds first), anything else means it died in front of the player.
 *
 * `bEnabled` is computed by the owner (config plus platform guards — this is off in the editor,
 * where PIE shutdowns are not real app deaths) so the class itself stays testable.
 *
 * ClearMarker is deliberately NOT gated on bEnabled: the provider must always be able to drop a
 * marker it cannot deliver, or a dirty exit found while consent is off would be reported forever.
 */
class FLOCK_API FFlockTerminationTracker
{
public:
	/** Injectable so tests can pin the death time without sleeping. */
	using FClock = TFunction<FDateTime()>;

	static const TCHAR* EventName;
	static const TCHAR* StateForeground;
	static const TCHAR* StateBackground;
	static const TCHAR* ClassBackgroundKill;
	static const TCHAR* ClassAbnormal;

	explicit FFlockTerminationTracker(bool bInEnabled, const FString& InMarkerPath = FString(),
		FClock InClock = FClock());

	/** `<ProjectSavedDir>/Flock/analytics/termination_marker.json`. */
	static FString DefaultMarkerPath();

	/** `background_kill` when it died backgrounded, `abnormal` otherwise. Empty for an invalid marker. */
	static FString Classify(const FFlockTerminationMarker& Marker);

	/**
	 * Reads a marker left by a previous run — call this BEFORE BeginTracking, which overwrites it.
	 * Returns false when there is none. A malformed marker is cleared so it cannot poison future
	 * launches, and also reports false.
	 *
	 * Ungated on bEnabled on purpose, so a marker written while the feature was on can still be
	 * drained after it is turned off.
	 */
	bool ReadSurvivingMarker(FFlockTerminationMarker& OutMarker) const;

	/** Starts leaving a tombstone for this run. No-op when disabled. */
	void BeginTracking(const FFlockTerminationMarker& Seed);

	/** The server session id arrives after the start call returns. */
	void SetServerSessionId(const FString& InServerSessionId);

	void SetBackgrounded(bool bBackgrounded);

	/** Counted into the marker on the next heartbeat rather than forcing a write per exception. */
	void NoteException();

	/** Refreshes the death-time estimate and folds in exceptions seen since the last one. */
	void HandleHeartbeat();

	/** The clean-exit path: stops tracking and removes the marker so the next boot sees nothing. */
	void StopTracking();

	/** Removes the marker without touching tracking state. Always allowed. */
	void ClearMarker() const;

	bool IsTracking() const { return bTracking; }
	int32 GetPendingExceptionCount() const { return PendingExceptionCount; }

private:
	void WriteMarker() const;

	bool bEnabled = false;
	FString MarkerPath;
	FClock Clock;

	FFlockTerminationMarker Marker;
	int32 PendingExceptionCount = 0;
	bool bTracking = false;
};
