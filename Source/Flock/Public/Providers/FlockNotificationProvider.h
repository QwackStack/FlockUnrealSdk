// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockNotificationModels.h"

/**
 * The signed-in player's notification inbox: list, unread count, summary, and the two mark-read writes.
 *
 * Every route here is player-scoped by its own schema — an inbox belongs to exactly one player — so all of
 * them fail fast with Auth when signed out. None of them declares `security` in the spec, so this is the
 * same deliberate carve-out as leaderboard /me and POST analytics/sessions: gated because the *schema* is
 * player-keyed, not because a 401 was once observed.
 *
 * Reads are snapshot-backed with **player-scoped keys**, so an inbox UI shows the last-known list when the
 * network is down, and one player's messages can never be served to the next player on a shared device.
 * Writes are never cached and never queued offline: a read-receipt replayed an hour later is worse than a
 * lost one, because it silently marks messages the player never saw.
 *
 * Completion-lambda rule: capture shared refs / weak self / values only — never `this`. Continuations that
 * re-enter the provider pin a TWeakPtr to itself, so teardown with requests in flight is safe.
 */
class FLOCK_API FFlockNotificationProvider
	: public FFlockProviderBase
	, public TSharedFromThis<FFlockNotificationProvider>
{
public:
	FFlockNotificationProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const TWeakObjectPtr<UFlockEvents>& InEvents,
		const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
		const FString& InGameVersionId);

	/**
	 * One page of the inbox, newest first. bUnreadOnly filters to unread. Page is 1-based.
	 *
	 * This is the one bare-shaped route in the family — {items,total,page,limit} at the root rather than
	 * under `result`. GetPaged copes with both, so nothing special happens here.
	 */
	void GetNotifications(bool bUnreadOnly, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockNotificationPage>)> OnComplete);

	/** The inbox with the usual defaults — everything, first page of 50. */
	void GetNotifications(TFunction<void(TFlockResult<FFlockNotificationPage>)> OnComplete)
	{
		GetNotifications(/*bUnreadOnly*/ false, 1, 50, MoveTemp(OnComplete));
	}

	/** How many unread messages the player has. The cheapest call for a badge count. */
	void GetUnreadCount(TFunction<void(TFlockResult<int32>)> OnComplete);

	/**
	 * Unread count plus the most recent few, in one call — the bell-icon payload. Limit caps the preview
	 * list, not the count.
	 */
	void GetSummary(int32 Limit, TFunction<void(TFlockResult<FFlockNotificationSummary>)> OnComplete);

	/** Summary with the default preview size. */
	void GetSummary(TFunction<void(TFlockResult<FFlockNotificationSummary>)> OnComplete)
	{
		GetSummary(10, MoveTemp(OnComplete));
	}

	/** Marks one notification read and answers with the updated row. Already-read is a success, not an error. */
	void MarkRead(const FString& NotificationId, TFunction<void(TFlockResult<FFlockNotification>)> OnComplete);

	/** Marks the whole inbox read. Updated is how many rows actually flipped; zero is a success. */
	void MarkAllRead(TFunction<void(TFlockResult<FFlockMarkAllReadResult>)> OnComplete);

	// Template catalog
	//
	// These two are the odd ones out in this provider: they declare no Authorization header and answer a
	// game-scoped schema, so they are **not gated on sign-in** and their cache is keyed by game version
	// rather than player. A catalog also does not mutate under the player the way an inbox does, so unlike
	// the inbox reads these are safe to memoize in process.

	/** Every template this game can schedule against. Memoized after the first call; snapshot-backed. */
	void GetTemplates(TFunction<void(TFlockResult<TArray<FFlockNotificationTemplate>>)> OnComplete);

	/**
	 * One template by name — what a designer actually has from the dashboard. Locale is optional; empty
	 * omits it and takes the template's default. An unknown name is a Validation failure, not an empty
	 * result: it is a caller mistake worth surfacing loudly.
	 */
	void GetTemplateByName(const FString& TemplateName, const FString& Locale,
		TFunction<void(TFlockResult<FFlockNotificationTemplate>)> OnComplete);

	/** By name in the default locale. */
	void GetTemplateByName(const FString& TemplateName, TFunction<void(TFlockResult<FFlockNotificationTemplate>)> OnComplete)
	{
		GetTemplateByName(TemplateName, FString(), MoveTemp(OnComplete));
	}

	// Scheduling

	/**
	 * Asks the backend to deliver a templated notification at DeliverAt, addressed by template **name**.
	 * Server-side, so it fires whether or not the game is running.
	 *
	 * This is the call a graph should reach for: a designer has a name from the dashboard, not a GUID. The
	 * name is resolved to an id through the by-name route and memoized for the session, the same shape as
	 * FlockLeaderboardProvider's board lookup — resolving a name must not cost a round trip on every send.
	 *
	 * Variables fill the template's placeholders; keys are the template's own, kept verbatim. An empty
	 * Channels list lets the server apply the template's defaults rather than forcing a choice here.
	 *
	 * **Not idempotent.** A retry after an ambiguous failure could leave the player with two of the same
	 * reminder, so this is never re-sent — the same rule the shop purchase follows, for the same reason.
	 */
	void ScheduleByTemplateName(const FString& TemplateName, const FDateTime& DeliverAtUtc,
		const FFlockCommandData& Variables, const TArray<EFlockNotificationChannel>& Channels,
		TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete);

	/** Schedules by name with no variables and the template's default channels. */
	void ScheduleByTemplateName(const FString& TemplateName, const FDateTime& DeliverAtUtc,
		TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete)
	{
		ScheduleByTemplateName(TemplateName, DeliverAtUtc, FFlockCommandData(), TArray<EFlockNotificationChannel>(), MoveTemp(OnComplete));
	}

	/**
	 * The same call addressed by template id, skipping the name lookup. For a caller that already holds an
	 * id — a re-schedule from a stored row, say. Named explicitly rather than overloading on FString, which
	 * would make the two indistinguishable at a call site.
	 */
	void ScheduleByTemplateId(const FString& TemplateId, const FDateTime& DeliverAtUtc,
		const FFlockCommandData& Variables, const TArray<EFlockNotificationChannel>& Channels,
		TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete);

	/**
	 * Cancels a scheduled notification and returns the canceled row. Idempotent — cancelling something
	 * already cancelled is not an error worth surfacing differently.
	 */
	void CancelScheduled(const FString& ScheduledId, TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete);

	/**
	 * The player's schedules **as the server knows them**, filtered by status.
	 *
	 * This is the authoritative listing and the one to prefer: unlike GetPendingSchedules it survives a
	 * reinstall and sees schedules made on another device, because it asks the backend rather than reading
	 * what this install happened to write down.
	 *
	 * Status is a string on purpose (see FlockScheduledNotificationStatuses) — the server owns the set. An
	 * empty Status omits the filter rather than sending a blank one.
	 *
	 * **Never cached, in process or on disk.** A schedule list changes whenever one is created, cancelled
	 * or delivered, and a delivery happens server-side with nothing to tell the client — so a cached page
	 * would go wrong rather than merely stale, the same call the inbox and the player inventory make.
	 * Offline it therefore fails, which is what GetPendingSchedules is for.
	 */
	void GetScheduled(const FString& Status, int32 Page, int32 Limit,
		TFunction<void(TFlockResult<FFlockScheduledNotificationPage>)> OnComplete);

	/** The pending schedules with the usual defaults — first page of 100. */
	void GetScheduled(TFunction<void(TFlockResult<FFlockScheduledNotificationPage>)> OnComplete)
	{
		GetScheduled(FlockScheduledNotificationStatuses::Pending, 1, 100, MoveTemp(OnComplete));
	}

	/**
	 * Schedules this install created that have not reached their delivery time yet.
	 *
	 * **Local bookkeeping, not a server query** — it knows only what *this install* scheduled, and infers
	 * delivery from the clock rather than being told. Entries whose time has passed are dropped as they
	 * are read. Synchronous, because it never touches the network.
	 *
	 * Now that the server can be asked, this is the **offline fallback** rather than the primary: prefer
	 * GetScheduled, and reach for this when a listing has to work without a network.
	 */
	TArray<FFlockPendingSchedule> GetPendingSchedules() const;

	/**
	 * Cancels every schedule the player has pending and reports how many the server actually cancelled.
	 *
	 * **The server's list is what gets cancelled**, so this also clears reminders made before a reinstall
	 * or on another device — the case that used to strand them permanently, since a scheduled notification
	 * still fires server-side with its id as the only handle. The local list is the fallback and is used
	 * only when that read fails; the two are never merged, because the server's answer is authoritative
	 * and a local entry it does not list is one it no longer considers pending.
	 *
	 * Entries the server no longer recognises (delivered, already cancelled, unknown) are **dropped rather
	 * than failing the batch** — they are not pending either way. A transient failure stops the run and
	 * fails, leaving the rest tracked so a later call can retry them; the count is not reported in that
	 * case, which is why this is for a "clear my reminders" action rather than a fire-and-forget sweep.
	 */
	void CancelAllScheduled(TFunction<void(TFlockResult<int32>)> OnComplete);

	// Push device tokens
	//
	// **The SDK never acquires a token, by design.** The game gets one from its push plugin — Firebase
	// Cloud Messaging, OneSignal — and hands the string here; this half registers it against the signed-in
	// player so the backend can deliver. That is where every comparable backend SDK draws the line, and on
	// Android it is the only line available: stock Unreal has no Java implementation behind its remote-
	// notification JNI hook, so there is nothing for the engine to hand over.
	//
	// Registering is idempotent — the row is keyed by token, so a retry after an ambiguous failure lands on
	// the same state. That is the opposite of scheduling, where a resend would double-book a reminder.

	/**
	 * Registers a push token for the signed-in player on an explicit platform.
	 *
	 * Use this when the token did not come from the running build — a web token from an embedded view, or a
	 * test harness. Prefer the auto-detecting overload otherwise.
	 */
	void RegisterDeviceToken(EFlockDevicePlatform Platform, const FString& Token,
		TFunction<void(TFlockResult<FFlockDeviceToken>)> OnComplete);

	/**
	 * Registers a push token, taking the platform from the running build.
	 *
	 * Fails **Validation** on anything the push backend does not accept — Windows, Mac, Linux, console and
	 * the editor. It never guesses a plausible platform, because a token registered under the wrong one
	 * fails silently at delivery time and looks like a backend fault weeks later.
	 */
	void RegisterDeviceToken(const FString& Token, TFunction<void(TFlockResult<FFlockDeviceToken>)> OnComplete);

	/** Deactivates a token so the backend stops delivering to it. Idempotent; Deactivated false means there was nothing to do. */
	void UnregisterDeviceToken(const FString& Token, TFunction<void(TFlockResult<FFlockUnregisterDeviceTokenResult>)> OnComplete);

	/**
	 * The platform this build would register under, if the push backend supports it. False on desktop,
	 * console and in the editor — call it to hide a "enable notifications" toggle rather than letting the
	 * register call fail.
	 */
	static bool GetCurrentDevicePlatform(EFlockDevicePlatform& OutPlatform);

	/**
	 * Drops **this player's** inbox cache, so the next read hits the backend. Called from Logout().
	 *
	 * Deliberately narrow, and the narrowness is the point. Three things it must not touch, each because
	 * touching them once lost real data: another player's records on a shared device (this used to delete
	 * the whole category); this player's seen-watermark and pending schedules (state, now in their own
	 * scope); and the template catalog (game-scoped — a title screen would refetch it after every sign-out).
	 *
	 * Signed out it does nothing at all. There is no player to clear for, and the version that resolved an
	 * empty player id into a scope wiped every account on the device.
	 */
	void ClearCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	/**
	 * "<version>/notification/<player id>" — this player's inbox cache, and the **only** thing ClearCache()
	 * deletes.
	 *
	 * The player belongs in the *scope*, not in the key, because a scope is the store's unit of deletion:
	 * with the player in the path, "drop this player's cache" is a directory delete that structurally cannot
	 * reach another player's. Suffixing keys instead — which this provider did — meant the delete had to be
	 * the whole shared category, and the only way to keep anything was to read it out first and write it
	 * back afterwards, which restores exactly one player's records and destroys everyone else's.
	 *
	 * **Empty when no player is signed in**, and every caller treats that as "nothing to do". A signed-out
	 * scope would collapse to the bare category (SanitizeScope culls empty segments) and take every player
	 * on the device with it.
	 */
	FString PlayerCacheScope() const;

	/**
	 * "<version>/notification_state/<player id>" — the seen-watermark and the pending-schedule list.
	 *
	 * A separate category because these are **state, not cache**: the watermark decides whether the next
	 * fetch re-announces an entire inbox, and the pending list is the only record of what this install
	 * scheduled — no `/v1` route can tell us again. Being outside the cache scope is what makes them survive
	 * ClearCache() by construction rather than by a read-then-restore, which also removes the ordering
	 * hazard that dance carried (it only worked because Logout() cleared caches before tokens).
	 *
	 * Empty when no player is signed in, same rule as PlayerCacheScope().
	 */
	FString PlayerStateScope() const;

	/**
	 * Moves a state entry written by an earlier version — which kept both under the shared cache category,
	 * keyed "<key>_<player id>" — into PlayerStateScope(), then deletes the old copy.
	 *
	 * Lazy, on read, and safe at any time: ClearCache() no longer deletes the old location either, since
	 * that location is the category root rather than the per-player scope. Returns true when it recovered
	 * something.
	 */
	bool TryMigrateLegacyState(const FString& Key, FString& OutPayload) const;

	/**
	 * Fails OnComplete with Auth when no player is signed in. Every route in this provider is player-scoped,
	 * so this runs first on all of them.
	 */
	template <typename T>
	bool RequireSignedIn(const TFunction<void(TFlockResult<T>)>& OnComplete) const
	{
		if (Session->IsAuthenticated())
		{
			return true;
		}
		if (OnComplete)
		{
			OnComplete(TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Auth, TEXT("No player is signed in"))));
		}
		return false;
	}

	/** Appends an optional query parameter, percent-encoded. Empty values are omitted, never sent blank. */
	static void AppendParam(FString& Query, const FString& Key, const FString& Value);

	/**
	 * Resolves a template name to an id and hands it to Continue, or fails. A name this game does not have
	 * is a caller mistake, so it is a Validation failure rather than an empty result.
	 */
	void WithTemplateId(const FString& TemplateName, TFunction<void(const FString&)> Continue,
		TFunction<void(const FFlockError&)> OnFailure);

	/**
	 * The one schedule path. Both public entry points funnel here so tracking happens in exactly one place;
	 * TemplateName is empty when the caller scheduled by id, which is the only thing the two differ on.
	 */
	void ScheduleInternal(const FString& TemplateId, const FString& TemplateName, const FDateTime& DeliverAtUtc,
		const FFlockCommandData& Variables, const TArray<EFlockNotificationChannel>& Channels,
		TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete);

	// Pending-schedule bookkeeping
	//
	// The id a schedule call returns is the only handle on a pending reminder and /v1 has no route to list
	// or read one back, so the SDK persists what it scheduled. Player-scoped: a second player on the same
	// device must not see, or be able to cancel, the first player's reminders.

	/** Stored entries with the elapsed ones dropped; rewrites the list when it drops any. */
	TArray<FFlockPendingSchedule> LoadPendingSchedules() const;

	/** Persists the list under the player-scoped key. No-op without a snapshot store. */
	void SavePendingSchedules(const TArray<FFlockPendingSchedule>& Pending) const;

	/** Records a successful schedule. Ignores a response with no id — there would be nothing to cancel. */
	void TrackPending(const FFlockScheduledNotification& Scheduled, const FString& TemplateName,
		const FString& TemplateId) const;

	/** Forgets one entry by scheduled id. Called after a cancel, and after a permanent rejection. */
	void UntrackPending(const FString& ScheduledId) const;

	/** One step of CancelAllScheduled's sequential walk. Kept a member so the recursion never captures `this`. */
	void CancelAllStep(TSharedRef<TArray<FString>> Ids, int32 Index, TSharedRef<int32> Cancelled,
		TFunction<void(TFlockResult<int32>)> OnComplete);

	/**
	 * Decides what CancelAllScheduled will cancel: the server's pending list, or this install's local one
	 * when that read fails.
	 *
	 * A fallback, never a merge. The server is authoritative about what is still pending, so an id it does
	 * not return is one it will not fire — cancelling it anyway spends a request to be told 404. The local
	 * list only earns a say when the server could not be reached at all.
	 */
	void ResolveCancellableScheduleIds(TFunction<void(TArray<FString>)> Continue);

	/**
	 * True when DeliverAt is in the past. An **unparseable** timestamp answers false, so the entry is kept:
	 * losing the only handle on a cancellable reminder is worse than carrying a stale row. (The opposite
	 * call from the watermark, where an unparseable date is skipped — there the risk is duplicate events
	 * forever, here it is a lost cancel.)
	 */
	static bool HasElapsed(const FString& DeliverAt);

	// Seen-watermark bookkeeping
	//
	// "Received" is fetch-derived: there is no realtime channel and the SDK never polls, so the watermark is
	// the only thing separating a notification the game has already been told about from a genuinely new one.

	/** The stored watermark for the signed-in player, or a fresh unseeded one when there is none. */
	FFlockNotificationWatermark LoadWatermark() const;

	/** Persists the watermark under the player-scoped key. No-op without a snapshot store. */
	void SaveWatermark(const FFlockNotificationWatermark& Mark) const;

	/**
	 * Raises OnNotificationReceived once per notification newer than the watermark, oldest first, then
	 * advances it. The first fetch for a player seeds silently — replaying an existing inbox as a burst of
	 * events is worse than not reporting its history.
	 */
	void RaiseNewNotifications(const TArray<FFlockNotification>& Items) const;

	/** Raises OnUnreadCountChanged. Only ever called with a count the server just reported. */
	void RaiseUnreadCount(int32 Count) const;

	TSharedRef<FFlockAuthSession> Session;

	/** Weak: the hub is a UObject owned by the subsystem and may be collected before this provider is. */
	TWeakObjectPtr<UFlockEvents> Events;

	FString VersionedApiUrl;

	/** Name -> template. Every name-addressed send resolves first, and that must not cost a round trip each time. */
	TMap<FString, FFlockNotificationTemplate> TemplatesByName;

	/** Inbox, counts and summary, under a per-player scope. This is what ClearCache() drops. */
	static const TCHAR* const SnapshotCategory;

	/**
	 * The seen-watermark and the pending-schedule list. Their own category because they are state rather
	 * than cache and must outlive a ClearCache() — see PlayerStateScope().
	 */
	static const TCHAR* const StateSnapshotCategory;

	/** Key of the seen-watermark within the state scope. */
	static const TCHAR* const WatermarkKey;

	/** Key of the pending-schedule list within the state scope. */
	static const TCHAR* const PendingSchedulesKey;

	/**
	 * Templates live in their own category because they are **game-scoped**, not player-scoped. Sharing the
	 * one category would mean logout deleted a catalog that has nothing to do with the departing player —
	 * and a title screen would refetch it on every sign-out.
	 */
	static const TCHAR* const TemplateSnapshotCategory;
};
