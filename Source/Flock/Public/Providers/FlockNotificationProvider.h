// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
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

	/** Drops the notification snapshot category, so the next read hits the backend. Called from Logout(). */
	void ClearCache();

private:
	FString MakeUrl(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Path); }
	TMap<FString, FString> HeadersNow() const { return Session->GetAuthHeaders(); }

	/** Suffixes a snapshot key with the current player id, so an inbox cannot cross accounts. */
	FString PlayerScopedKey(const FString& Key) const;

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

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;

	/** Name -> template. Every name-addressed send resolves first, and that must not cost a round trip each time. */
	TMap<FString, FFlockNotificationTemplate> TemplatesByName;

	/** Player-scoped keys: inbox, counts, summary. Dropped wholesale on logout. */
	static const TCHAR* const SnapshotCategory;

	/**
	 * Templates live in their own category because they are **game-scoped**, not player-scoped. Sharing the
	 * one category would mean logout deleted a catalog that has nothing to do with the departing player —
	 * and a title screen would refetch it on every sign-out.
	 */
	static const TCHAR* const TemplateSnapshotCategory;
};
