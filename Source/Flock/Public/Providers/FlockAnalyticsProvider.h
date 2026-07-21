// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Analytics/FlockAnalyticsConfig.h"
#include "Analytics/FlockConsentStore.h"
#include "Analytics/FlockEventCache.h"
#include "Analytics/FlockLifecyclePump.h"
#include "Analytics/FlockSession.h"
#include "Analytics/FlockTerminationTracker.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEventModels.h"
#include "FlockEvents.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockAnalyticsModels.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FFlockLogSink;

/**
 * The collaborators the provider drives. Supplied by the subsystem in production and substituted in
 * tests, which is what keeps the provider's own tests free of disk and network.
 */
struct FLOCK_API FFlockAnalyticsDependencies
{
	TSharedPtr<IFlockEventCache> LogEventCache;
	TSharedPtr<FFlockSession> Session;
	TSharedPtr<FFlockTerminationTracker> TerminationTracker;
	TSharedPtr<FFlockConsentStore> ConsentStore;
	TSharedPtr<FFlockLifecyclePump> Pump;

	/**
	 * Tapping GLog is off by default because a sink running inside the automation runner captures
	 * the runner's own error output. The subsystem turns it on.
	 */
	bool bEnableLogSink = false;
};

/**
 * Analytics: the log_event API (event / error / exception), session lifecycle, heartbeat, offline
 * spool with explicit flush, consent gating, and next-launch app_termination reporting.
 *
 * This is the only piece that knows about all the others. The cache stores, the session counts, the
 * pump ticks, the tracker tombstones, the sink captures — none of them talk to each other, and every
 * policy decision (when to flush, when to rotate a session, what a tick means) lands here.
 *
 * Consent is a hard gate, not a send filter: with consent withheld there is no session and nothing is
 * collected, not even locally. Revoking ends the session and drops what was spooled.
 *
 * Delivery is fire-and-forget by design — every log_event route answers with a free-form body nobody
 * reads, so success is the 2xx. Entries stay spooled until the send is acknowledged, which is what
 * makes a crash or an offline stretch cost nothing.
 *
 * Completion-lambda rule: capture shared refs / weak ptrs / values only — never `this`. Continuations
 * that must re-enter the provider pin a TWeakPtr to itself and give up if it has gone.
 */
class FLOCK_API FFlockAnalyticsProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockAnalyticsProvider>
{
public:
	FFlockAnalyticsProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const TWeakObjectPtr<UFlockEvents>& InEvents, const FString& InVersionedApiUrl,
		const FFlockAnalyticsConfig& InConfig, const FFlockAnalyticsDependencies& InDependencies,
		const FString& InGameVersion, const FString& InSdkVersion);

	virtual ~FFlockAnalyticsProvider() override;

	/**
	 * Drains any app_termination left by the previous run, starts the pump and the log sink, and
	 * begins tombstoning this one. Does not start a session — that needs a player id.
	 */
	void Initialize();

	/** Ends the session if configured to, stops the pump and sink, and clears the tombstone. */
	void Shutdown();

	// ── Consent ──

	bool HasConsent() const;

	/**
	 * Granting starts a session when a player is known and auto-start is on. Revoking ends the
	 * session and drops the spool — a player who opts out should not leave queued data behind.
	 * Raises UFlockEvents::OnConsentChanged when the value actually changes.
	 */
	void SetConsent(bool bGranted);

	// ── Sessions ──

	/** Requires a player id: the backend makes it mandatory, so sessions begin after sign-in. */
	void StartSession(const FString& InPlayerId, TFunction<void(TFlockResult<FString>)> OnComplete = nullptr);

	void EndSession(EFlockSessionEndReason Reason = EFlockSessionEndReason::Manual,
		TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete = nullptr);

	bool HasActiveSession() const;
	FString GetCurrentSessionId() const;
	FFlockSessionSnapshot GetCurrentSnapshot() const;

	// ── log_event API ──
	// All three spool immediately and deliver on the next flush; none of them block or report failure,
	// because an analytics call must never be a reason the game stops.

	void LogEvent(const FString& Message, const TMap<FString, FString>& ExtraData = TMap<FString, FString>());

	void LogError(const FString& Message, const FString& LogicalExpression = FString(),
		const FString& ErrorCode = FString(), const TMap<FString, FString>& ErrorData = TMap<FString, FString>(),
		const TMap<FString, FString>& ExtraData = TMap<FString, FString>());

	void LogException(const FString& Message, const FString& StackTrace,
		const TMap<FString, FString>& ErrorData = TMap<FString, FString>(),
		const TMap<FString, FString>& ExtraData = TMap<FString, FString>());

	void RecordScreenView(const FString& ScreenName);

	/** Drains the spool batch by batch until it is empty or a send fails. */
	void Flush(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete = nullptr);

	/** Drops the spool, the consent decision, and any tombstone — the "erase my data" path. */
	void EraseLocalData();

	int32 GetPendingEventCount() const;

	/** Test seam: run one tick's worth of policy without an engine loop. */
	void TickForTesting(float DeltaSeconds);

	/**
	 * Test seam: the live log sink, so a test can drive a captured error through the real automatic
	 * path (sink -> tick drain -> spooled exception) instead of trusting the wiring by inspection.
	 * Null unless the provider was built with bEnableLogSink.
	 */
	FFlockLogSink* GetLogSinkForTesting() const { return LogSink.Get(); }

private:
	FString AnalyticsUrl(const FString& Endpoint) const;

	bool IsCollecting() const;

	void QueueLogEvent(FFlockLogEventRequest&& Event);
	FFlockLogEventRequest MakeLogEvent(EFlockLogEventType Type, const FString& Message) const;

	void HandleTick(float DeltaSeconds);
	void HandleBackgroundChanged(bool bBackgrounded);
	void HandleQuit();

	void DrainLogSink();
	void ReportSurvivingTermination();
	void SendNextBatch(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);

	FFlockAnalyticsConfig Config;
	FFlockAnalyticsDependencies Deps;
	TSharedRef<FFlockAuthSession> AuthSessionRef;
	TWeakObjectPtr<UFlockEvents> Events;
	FString VersionedApiUrl;
	FString GameVersion;
	FString SdkVersion;

	TSharedPtr<FFlockLogSink> LogSink;

	/**
	 * The last player id handed to StartSession, remembered even when consent refused the session.
	 * That is what lets a later grant open the session that could not open at sign-in — the opt-in
	 * flow's whole point.
	 */
	FString KnownPlayerId;

	float HeartbeatAccumulator = 0.f;
	float FlushAccumulator = 0.f;
	bool bInitialized = false;
	bool bFlushInFlight = false;
	bool bConsent = false;
};
