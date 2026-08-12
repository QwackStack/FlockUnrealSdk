// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "Http/FlockProviderBase.h"
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

	TSharedRef<FFlockAuthSession> Session;
	FString VersionedApiUrl;

	static const TCHAR* const SnapshotCategory;
};
