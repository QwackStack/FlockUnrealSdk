// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Analytics/FlockAnalyticsConfig.h"
#include "Analytics/FlockConsentStore.h"
#include "Analytics/FlockEventCache.h"
#include "Analytics/FlockRepeatedExceptionCounter.h"
#include "Analytics/FlockLifecyclePump.h"
#include "Analytics/FlockSession.h"
#include "Analytics/FlockTerminationTracker.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEventModels.h"
#include "FlockEvents.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockAnalyticsModels.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FFlockAnalyticsLaunches;
class FFlockLogSink;
struct FFlockSpooledAnalyticsEvent;

/**
 * The collaborators the provider drives. Supplied by the subsystem in production and substituted in
 * tests, which is what keeps the provider's own tests free of disk and network.
 */
struct FLOCK_API FFlockAnalyticsDependencies
{
	/**
	 * This launch's analytics folder, held for the provider's life, and the launches that had ended when it started. Their crash
	 * markers and live-session records are reported once at Initialize, and then those launches are deleted. Null reports
	 * nothing from earlier launches.
	 */
	TSharedPtr<FFlockAnalyticsLaunches> Launches;

	TSharedPtr<IFlockEventCache> LogEventCache;

	/**
	 * Gameplay events for `POST analytics/events`. Its own instance with its own cap, so a burst of one stream can
	 * never evict the other's entries, and not scoped by game version, so a version bump cannot delete unsent events.
	 */
	TSharedPtr<IFlockEventCache> AnalyticsEventCache;

	/**
	 * Durable queue of session ends, separate from the log spool so ends can drain first and so
	 * erasing one does not disturb the other. Without it an end that cannot be delivered at the
	 * moment it happens — offline, mid-quit, signed out — is lost.
	 */
	TSharedPtr<IFlockEventCache> SessionEndCache;

	TSharedPtr<FFlockSession> Session;
	TSharedPtr<FFlockTerminationTracker> TerminationTracker;
	TSharedPtr<FFlockConsentStore> ConsentStore;
	TSharedPtr<FFlockLifecyclePump> Pump;

	/**
	 * Tapping GLog is off by default because a sink running inside the automation runner captures
	 * the runner's own error output. The subsystem turns it on.
	 */
	bool bEnableLogSink = false;

	/**
	 * Where the coverage notice records the build it was sent for, so a build that cannot see every class of fault
	 * says so once rather than on every launch. Empty sends it once per initialization.
	 */
	FString CoverageNoticeMarkerPath;
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

	/**
	 * The backend requires a player id, so sessions begin after sign-in. Leave InPlayerId empty to
	 * use the signed-in player — the SDK already knows who that is, so making every caller fetch and
	 * pass it was pure ceremony.
	 */
	void StartSession(const FString& InPlayerId = FString(),
		TFunction<void(TFlockResult<FString>)> OnComplete = nullptr);

	/**
	 * Closes the session and delivers its end. The end is made durable before anything is sent, so a
	 * failure here costs nothing: the record drains on a later flush, or on the next launch.
	 */
	void EndSession(EFlockSessionEndReason Reason = EFlockSessionEndReason::Manual,
		TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete = nullptr);

	/**
	 * The player signed out. Closes the session if one is still open and forgets the remembered
	 * player, so a consent grant arriving afterwards cannot open a session attributed to whoever just
	 * left. One call rather than two, because the order of those two steps is analytics' business,
	 * not the caller's.
	 */
	void HandleLoggedOut();

	bool HasActiveSession() const;
	FString GetCurrentSessionId() const;
	FFlockSessionSnapshot GetCurrentSnapshot() const;

	// ── log_event API ──
	// All three spool immediately and deliver on the next flush; none of them block or report failure,
	// because an analytics call must never be a reason the game stops.

	/**
	 * A diagnostic entry. Not for gameplay — a level completed or an item bought is TrackEvent.
	 *
	 * Surface: log_event — read on Diagnostics → Events.
	 */
	void LogEvent(const FString& Message, const TMap<FString, FString>& ExtraData = TMap<FString, FString>());

	/**
	 * A recoverable logic fault.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	void LogError(const FString& Message, const FFlockLogDetails& Details = FFlockLogDetails());

	/**
	 * Leave StackTrace empty and the SDK walks the callstack itself — which is almost always what
	 * you want, because a caller rarely has one to hand. Requiring it produced exactly the failure
	 * mode you would expect: callers passing a placeholder string, and exception reports arriving
	 * with no diagnostic value.
	 *
	 * Pass a trace only when you genuinely have a better one (a script VM's stack, say) than the
	 * native callstack at this point.
	 *
	 * Surface: log_event — read on Diagnostics → Errors.
	 */
	void LogException(const FString& Message, const FString& StackTrace = FString(),
		const FFlockLogDetails& Details = FFlockLogDetails());

	/**
	 * Counts a screen view against the current session.
	 *
	 * Surface: analytics — read on Dashboards → Game Metrics.
	 */
	void RecordScreenView(const FString& ScreenName);

	// ── Analytics events ──

	/** Reserved: the server records this itself when `POST analytics/sessions` succeeds. */
	static constexpr const TCHAR* ReservedSessionStartedEvent = TEXT("session_started");

	/** The longest event name the server can store. It fails the whole request for an event with a longer one. */
	static constexpr int32 MaxEventNameLength = 200;

	/** The longest event category the server can store. It fails the whole request for an event with a longer one. */
	static constexpr int32 MaxEventCategoryLength = 100;

	/** True when the server could not store an event with this name or category. */
	static bool IsEventTooLongToStore(const FString& EventName, const FString& EventCategory);

	/**
	 * Answered failed sends a spooled entry survives before it is dropped. Unanswered ones — offline, a timeout —
	 * do not count: a flush fires every interval, so counting those would discard data after minutes offline.
	 */
	static constexpr int32 MaxFailedSends = 50;

	/**
	 * The longest the periodic flush waits after failures the server answered. Each one spends an attempt from every
	 * entry it carried, so at a fixed interval an outage of minutes would spend a whole budget. Doubling the wait up
	 * to this makes the same budget last hours, while an entry the server keeps failing on is still dropped in the end.
	 */
	static constexpr float MaxFlushWaitSeconds = 900.f;

	/**
	 * Records a gameplay event for the Game Metrics dashboards (`level_complete`, `purchase_view`). Not LogEvent,
	 * which writes a diagnostic entry to a different dashboard.
	 *
	 * Never touches the network: the event is spooled and delivered on a flush while a player is signed in, so it
	 * is safe on a hot path and while offline. With nobody signed in it is held and credited to whoever signs in
	 * next — the server refuses an event with no player. Properties keep their keys verbatim and their JSON types.
	 *
	 * Returns false when refused: analytics off, no consent, an empty name, a name longer than MaxEventNameLength or a
	 * category longer than MaxEventCategoryLength (the server cannot store either), or `session_started`, which the
	 * server writes itself and would otherwise count twice. Called off the game thread it is forwarded there
	 * and answers true, since it cannot be judged from the calling thread.
	 *
	 * Surface: analytics — read on Dashboards → Game Metrics.
	 */
	bool TrackEvent(const FString& EventName, const FFlockCommandData& Properties = FFlockCommandData(),
		const FString& EventCategory = FString());

	/** Gameplay events still spooled, held ones included. */
	int32 GetPendingAnalyticsEventCount() const;

	/**
	 * A player signed in or a session was restored. Events recorded while nobody was signed in are attributed to
	 * this player and a delivery starts. An event that already carries a player keeps it.
	 */
	void HandleAuthenticated(const FString& PlayerId);

	/** What automatic exception capture can see in this build. */
	FFlockExceptionCaptureCoverage GetExceptionCaptureCoverage() const;

	/** The coverage rules, apart from the macros they are read from, so each can be tested. */
	static FFlockExceptionCaptureCoverage ComputeCoverage(bool bCaptureEnabled, bool bLoggingCompiledIn,
		bool bEnsuresCompiledIn, bool bBlueprintGuardCompiledIn, const FString& BuildConfiguration);

	// ── Transactions ──

	/**
	 * Records a monetary transaction (`POST analytics/transactions`) for revenue/LTV metrics.
	 * Unlike the log_event calls this is sent immediately, not spooled, and it requires a signed-in
	 * player — the backend needs a player id, so a pre-auth call fails with Auth rather than being
	 * queued. PlayerId, SessionId and CreatedAt are filled from the current session when left empty.
	 * A negative Amount is rejected as Validation. The shop provider drives this around a purchase
	 * (best-effort); games may also call it directly.
	 *
	 * Surface: analytics — read on Dashboards → Game Metrics.
	 */
	void RecordTransaction(const FFlockAnalyticsTransactionRequest& Request,
		TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete = nullptr);

	/**
	 * Drains everything queued, batch by batch, until it is empty or a send fails. Session ends go
	 * first — they are small, rare, and the most valuable record, so a quit's remaining time must not
	 * be spent on an event backlog ahead of them.
	 *
	 * A failed *send* is reported as a failure, and so is an entry the server refused: that entry is dropped and
	 * the rest are still delivered, but the pass reports the refusal, because something was lost. Deferral is
	 * not a failure: with nothing queued, a drain already running, or nobody signed in, this succeeds without
	 * sending anything, because none of those is a reason for a fire-and-forget flush to look broken. Success
	 * therefore means "nothing was lost", not "the queue is now empty" — read GetPendingEventCount() if you need
	 * the latter.
	 *
	 * Gameplay events are delivered only while a player is signed in; otherwise they wait for the next sign-in.
	 */
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

	/** Test seam: the clock repeat windows are measured against. */
	void SetClockForTesting(TFunction<double()> InClock) { Clock = MoveTemp(InClock); }

	/** Test seam: stands in for the build's compile-time coverage, so a Development test can see a Shipping answer. */
	void SetCoverageForTesting(const FFlockExceptionCaptureCoverage& InCoverage) { CoverageOverride = InCoverage; }

	/**
	 * Test seam: spools a gameplay event under an explicit player, which the public API never does. The live
	 * counter-case for a player the server does not know rides on it.
	 */
	void TrackEventAsPlayerForTesting(const FString& PlayerId, const FString& EventName);

	/** Test seam: how long the periodic flush currently waits beyond its interval; 0 when it does not. */
	float GetFlushWaitSecondsForTesting() const { return FlushWaitSeconds; }

private:
	/** One delivery pass over a spool: the log entries or the gameplay events. */
	struct FDeliveryPass
	{
		/** A 409 is answered once, by resending without session ids; a second one is a refusal. */
		bool bRetriedWithoutSession = false;

		/** Entries still to send one at a time, after the server refused the content of the batch they were in. */
		int32 EntriesToSendOneAtATime = 0;

		/** The first refusal entries were dropped for. The pass still delivers the rest, then reports it. */
		TOptional<FFlockError> FirstRefusal;

		int32 NextBatchSize(int32 Configured) const { return EntriesToSendOneAtATime > 0 ? 1 : FMath::Max(Configured, 1); }

		/** An entry left the spool, delivered or unreadable. */
		void NoteEntryDone()
		{
			if (EntriesToSendOneAtATime > 0)
			{
				--EntriesToSendOneAtATime;
			}
		}

		void NoteEntryRefused(const FFlockError& Error)
		{
			NoteEntryDone();
			if (!FirstRefusal.IsSet())
			{
				FirstRefusal = Error;
			}
		}

		TFlockResult<FFlockAnalyticsAck> ResultToReport() const
		{
			return FirstRefusal.IsSet() ? TFlockResult<FFlockAnalyticsAck>::Fail(FirstRefusal.GetValue())
				: TFlockResult<FFlockAnalyticsAck>::Ok(FFlockAnalyticsAck());
		}
	};
	FString AnalyticsUrl(const FString& Endpoint) const;

	bool IsCollecting() const;

	void QueueLogEvent(FFlockLogEventRequest&& Event);
	FFlockLogEventRequest MakeLogEvent(EFlockLogEventType Type, const FString& Message) const;

	void HandleTick(float DeltaSeconds);
	void HandleBackgroundChanged(bool bBackgrounded);
	void HandleQuit();

	void DrainLogSink();

	/** Reports the crash marker and spools the live session of every launch that had ended, then deletes those launches. */
	void ReportWhatEndedLaunchesLeft();

	/** Reports the marker at MarkerPath, if an ended launch left one, and deletes it. */
	void ReportSurvivingTermination(const FString& MarkerPath);

	/** Builds and spools an exception entry from a trace already in hand. LogException adds the walk and the tally. */
	void SpoolException(const FString& Message, const FString& StackTrace, const FFlockLogDetails& Details);

	/** Spools one entry per repeat window that closed with repeats in it. */
	void QueueRepeatReports(const TArray<FFlockRepeatedExceptionCounter::FRepeatReport>& Summaries);

	/** Spools the once-per-initialization notice when this build cannot see every class of fault. */
	void ReportCaptureCoverage();

	double NowSeconds() const;

	/** Spends one attempt on each log entry of a batch the server answered but did not accept. */
	void CountFailedLogSends(const TArray<FString>& Handles, const TArray<FString>& Payloads);

	/** A refusal of a body's content, which a single entry can cause: the batch is worth taking apart for it. */
	static bool IsRefusalOfContent(const FFlockError& Error);

	/** Stretches or resets the periodic flush's wait from what the last one met. */
	void UpdateFlushWait(const TFlockResult<FFlockAnalyticsAck>& Result);

	void FlushAnalyticsEvents(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);
	void SendNextEventBatch(const TSharedRef<FDeliveryPass>& Pass, TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);
	void HandleEventBatchFailure(const TSharedRef<FDeliveryPass>& Pass, const TArray<FString>& Handles,
		const TArray<FFlockSpooledAnalyticsEvent>& Entries, const FString& BatchPlayer, const FFlockError& Error,
		TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);

	// ── Session internals ──

	/** Body for `POST analytics/sessions`, from a live or a spooled snapshot. */
	FFlockSessionStartRequest MakeStartRequest(const FFlockSessionSnapshot& Snapshot) const;

	/**
	 * Registers the live session, single-flight. The id is adopted only if that same session is still
	 * the active one when the reply lands — it may have rotated or ended while the POST was away, and
	 * tagging the new session with the old one's id corrupts both.
	 */
	void RegisterActiveSession(TFunction<void(TFlockResult<FString>)> OnComplete);

	/** Heartbeat hook: registers an active session that has none yet. */
	void TryHealRegistration();

	/**
	 * Marks the one failure the spool is designed around — an auth failure while nobody is signed in
	 * — as expected, so it logs as debug instead of burying real errors. Deliberately not "any 401":
	 * one that arrives with a live bearer has already survived the provider base's silent refresh,
	 * and that is a genuine problem worth seeing.
	 */
	TFunction<bool(const FFlockError&)> SignedOutFailurePredicate() const;

	/** Closes the session and clears what it left behind locally: live record and crash tombstone. */
	void StopSessionLocally();

	/**
	 * The one end path. Closes the session, makes the end durable, stops the tombstone, and raises
	 * OnSessionEnded. bOutSpooled is false when there was no spool to take it, and the caller must
	 * deliver the returned snapshot itself or lose it.
	 */
	FFlockSessionSnapshot FinishSession(EFlockSessionEndReason Reason, bool& bOutSpooled);

	/** Consent-revoke path: stops the session locally with no spool, no send, and no OnSessionEnded. */
	void DiscardSession();

	/**
	 * Spools the end of the session an ended launch left open in the record at SessionStatePath, and carries its session number
	 * on. False only when there was an end to keep and it could not be spooled, so that launch is kept for a later one.
	 */
	bool RecoverOrphanedSession(const FString& SessionStatePath);

	/**
	 * IsExpectedFailure is supplied by the spool drain, where a signed-out failure is a wait rather
	 * than a loss. The no-spool path leaves it unset: there the record really does go missing.
	 */
	void PatchSessionEnd(const FString& ServerSessionId, const FFlockSessionSnapshot& Snapshot,
		TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete,
		TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr);

	void FlushSessionEnds(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);
	/** Delivered carries the session ids already sent in this pass, so a duplicate record is dropped. */
	void SendNextEnd(const TSharedRef<TSet<FString>>& Delivered,
		TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);
	void SendSpooledEnd(const FString& Handle, const FFlockSessionSnapshot& Snapshot,
		const TSharedRef<TSet<FString>>& Delivered, TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);
	/** Drops the entry and carries on when the failure is final; otherwise stops the pass. */
	void HandleEndFailure(const FString& Handle, const FString& SessionId, const FFlockError& Error,
		const TSharedRef<TSet<FString>>& Delivered, TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);

	void FlushLogEvents(TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);
	void SendNextBatch(const TSharedRef<FDeliveryPass>& Pass, TFunction<void(TFlockResult<FFlockAnalyticsAck>)> OnComplete);

	FFlockAnalyticsConfig Config;
	FFlockAnalyticsDependencies Deps;
	TSharedRef<FFlockAuthSession> AuthSessionRef;
	TWeakObjectPtr<UFlockEvents> Events;
	FString VersionedApiUrl;
	FString GameVersion;
	FString SdkVersion;

	TSharedPtr<FFlockLogSink> LogSink;

	/** Applies to automatically captured faults only; a manual LogException is always recorded. */
	FFlockRepeatedExceptionCounter RepeatedExceptions;

	TFunction<double()> Clock;
	TOptional<FFlockExceptionCaptureCoverage> CoverageOverride;

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
	bool bEndFlushInFlight = false;
	/** Its own flag: a log batch in flight must not stop gameplay events being delivered, nor the reverse. */
	bool bEventsFlushInFlight = false;

	/** Stretched by failures the server answered, reset by a flush that leaves nothing behind. */
	float FlushWaitSeconds = 0.f;

	/**
	 * Server session ids a batch was refused for, and the only record of them: checked when an event is recorded and
	 * again when a batch is sent. The live session keeps its id for its own end, but no event carries one of these
	 * into another refusal.
	 */
	TSet<FString> RefusedSessionIds;
	/** A second POST while one is away would open a second server session for the same play session. */
	bool bRegistrationInFlight = false;
	bool bConsent = false;
};
