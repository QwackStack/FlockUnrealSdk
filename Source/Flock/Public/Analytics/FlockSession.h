// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Analytics/FlockAnalyticsConfig.h"
#include "Models/FlockAnalyticsModels.h"

/**
 * One gameplay session's state and accounting: duration, pauses, screen views, FPS.
 *
 * Deliberately inert — it owns no timers, raises no delegates, and sends nothing. It is driven from
 * outside (the pump ticks it, the provider decides when to heartbeat, rotate, or end it), which is
 * what makes the whole of it testable without an engine loop or a network.
 *
 * Duration accumulates from tick deltas rather than subtracting wall-clock stamps, so a device
 * clock that jumps mid-session (timezone change, NTP correction, a player fiddling with the date)
 * cannot corrupt it. Wall-clock is used only for the timestamps the backend wants.
 *
 * Game thread only — no locking.
 */
class FLOCK_API FFlockSession
{
public:
	/** Injectable so tests can advance wall-clock time without sleeping. Defaults to FDateTime::UtcNow. */
	using FClock = TFunction<FDateTime()>;

	/** Screen names kept per session; the view *count* is uncapped. */
	static constexpr int32 MaxTrackedScreenNames = 100;

	/** An empty InStateFilePath uses DefaultStatePath(). */
	explicit FFlockSession(const FFlockAnalyticsConfig& InConfig, const FString& InStateFilePath = FString(),
		FClock InClock = FClock());

	/** `<ProjectSavedDir>/Flock/analytics/session_state.json` — carries the session counter across runs. */
	static FString DefaultStatePath();

	// ── Lifecycle ──

	/** Starts a session and returns its local id. A no-op returning the current id when already active. */
	FString Start(const FString& InPlayerId);

	/** Closes the session. Safe to call when already ended; closes an open pause first. */
	void End();

	bool IsActive() const { return bActive; }
	bool IsPaused() const { return bPaused; }

	// ── Driving ──

	/** Accumulates duration and FPS. Ignored while paused or inactive. */
	void Tick(float DeltaSeconds);

	/** Backgrounded. Stops duration accruing and counts a pause. */
	void Pause();

	/**
	 * Foregrounded. Returns true when the app was away longer than SessionTimeoutSeconds, meaning
	 * the caller should end this session and start a fresh one rather than resume.
	 */
	bool Resume();

	void RecordScreenView(const FString& ScreenName);

	/** Stamps LastHeartbeatUtc. The interval itself is the provider's business. */
	void MarkHeartbeat();

	// ── State ──

	const FString& GetSessionId() const { return SessionId; }
	const FString& GetServerSessionId() const { return ServerSessionId; }
	void SetServerSessionId(const FString& InServerSessionId) { ServerSessionId = InServerSessionId; }

	const FString& GetPlayerId() const { return PlayerId; }
	/** Retag after a late sign-in, so a session opened while signed out attributes correctly. */
	void SetPlayerId(const FString& InPlayerId) { PlayerId = InPlayerId; }

	int32 GetSessionNumber() const { return SessionNumber; }
	float GetDurationSeconds() const { return DurationSeconds; }
	int32 GetScreensViewed() const { return ScreensViewed; }

	/** True when the session is shorter than the configured bounce threshold. */
	bool IsBounce() const;

	FFlockSessionSnapshot TakeSnapshot() const;

private:
	void LoadSessionNumber();
	void SaveSessionNumber() const;
	void ResetAccounting();
	void SampleFps(float DeltaSeconds);

	FFlockAnalyticsConfig Config;
	FString StateFilePath;
	FClock Clock;

	FString SessionId;
	FString ServerSessionId;
	FString PlayerId;
	int32 SessionNumber = 0;

	FDateTime StartTimeUtc = FDateTime::MinValue();
	FDateTime EndTimeUtc = FDateTime::MinValue();
	FDateTime LastHeartbeatUtc = FDateTime::MinValue();
	FDateTime PauseStartUtc = FDateTime::MinValue();

	float DurationSeconds = 0.f;
	float TotalPauseDurationSeconds = 0.f;
	int32 PauseCount = 0;

	int32 ScreensViewed = 0;
	TArray<FString> ScreenNames;

	float FpsWindowSeconds = 0.f;
	int32 FpsWindowFrames = 0;
	float FpsSampleTotal = 0.f;
	int32 FpsSampleCount = 0;
	float MinFps = 0.f;
	float MaxFps = 0.f;

	bool bActive = false;
	bool bPaused = false;
};
