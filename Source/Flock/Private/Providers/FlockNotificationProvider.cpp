// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockNotificationProvider.h"

#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"

const TCHAR* const FFlockNotificationProvider::SnapshotCategory = TEXT("notification");

FFlockNotificationProvider::FFlockNotificationProvider(const TSharedRef<FFlockHttpClient>& InClient,
	const FFlockRetryPolicy& InPolicy, const TSharedRef<IFlockLogger>& InLogger,
	const TSharedRef<FFlockAuthSession>& InSession, const FString& InVersionedApiUrl,
	const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore, const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
	SetAuthSession(InSession);
}

void FFlockNotificationProvider::AppendParam(FString& Query, const FString& Key, const FString& Value)
{
	// Optional filters are omitted rather than sent empty, so the server never has to guess whether a blank
	// value meant "all" or "none".
	if (Value.IsEmpty())
	{
		return;
	}
	Query += Query.IsEmpty() ? TEXT("?") : TEXT("&");
	Query += Key + TEXT("=") + FlockEndpoints::Encode(Value);
}

FString FFlockNotificationProvider::PlayerScopedKey(const FString& Key) const
{
	return FString::Printf(TEXT("%s_%s"), *Key, *Session->GetPlayerId());
}

// ─────────────────────────────────── Reads ───────────────────────────────────

void FFlockNotificationProvider::GetNotifications(bool bUnreadOnly, int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockNotificationPage>)> OnComplete)
{
	if (!RequireSignedIn<FFlockNotificationPage>(OnComplete))
	{
		return;
	}

	// No in-process page memo, unlike the shop catalog: an inbox mutates under the player's feet (a new
	// message arrives, a mark-read flips a flag), so a memoized page would go wrong rather than stale.
	// The snapshot is still worth keeping — it only ever serves when the fetch could not.
	FString Query;
	Query += TEXT("?page=") + FString::FromInt(Page);
	Query += TEXT("&limit=") + FString::FromInt(Limit);
	if (bUnreadOnly)
	{
		Query += TEXT("&unread_only=true");
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s%s"), FlockEndpoints::Notification, *Query));
	const TMap<FString, FString> Headers = HeadersNow();
	const FString Key = PlayerScopedKey(FString::Printf(TEXT("inbox_p%d_l%d_u%d"), Page, Limit, bUnreadOnly ? 1 : 0));

	FetchWithSnapshot<FFlockNotificationPage>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockNotificationPage>)> OnAttempt)
		{
			// **Bare** route: {items,total,page,limit} at the root, not under `result`. UnwrapPaginated
			// starts at the root and only descends into `result` when present, so GetPaged serves both
			// shapes and no raw verb is needed here.
			return ClientRef->GetPaged<FFlockNotification>(Url, Headers,
				[OnAttempt](TFlockResult<TFlockPage<FFlockNotification>> Result)
				{
					if (!Result.bSuccess)
					{
						OnAttempt(TFlockResult<FFlockNotificationPage>::Fail(Result.Error));
						return;
					}
					FFlockNotificationPage Page;
					Page.Items = Result.Value.Items;
					Page.Total = Result.Value.Total;
					Page.Page = Result.Value.Page;
					Page.Limit = Result.Value.Limit;
					OnAttempt(TFlockResult<FFlockNotificationPage>::Ok(Page));
				});
		},
		TEXT("Fetch notifications"),
		MoveTemp(OnComplete));
}

void FFlockNotificationProvider::GetUnreadCount(TFunction<void(TFlockResult<int32>)> OnComplete)
{
	if (!RequireSignedIn<int32>(OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::NotificationUnreadCount);
	const TMap<FString, FString> Headers = HeadersNow();

	FetchWithSnapshot<FFlockUnreadCount>(SnapshotCategory, PlayerScopedKey(TEXT("unread_count")),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockUnreadCount>)> OnAttempt)
		{
			// Enveloped ({error,response,result}) — the enveloped verb unwraps `result`.
			return ClientRef->Get<FFlockUnreadCount>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch unread count"),
		[OnComplete](TFlockResult<FFlockUnreadCount> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			// The wire carries a one-field object; callers want the number, so the struct stays internal.
			OnComplete(Result.bSuccess
				? TFlockResult<int32>::Ok(Result.Value.Count)
				: TFlockResult<int32>::Fail(Result.Error));
		});
}

void FFlockNotificationProvider::GetSummary(int32 Limit, TFunction<void(TFlockResult<FFlockNotificationSummary>)> OnComplete)
{
	if (!RequireSignedIn<FFlockNotificationSummary>(OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s?limit=%d"), FlockEndpoints::NotificationSummary, Limit));
	const TMap<FString, FString> Headers = HeadersNow();

	FetchWithSnapshot<FFlockNotificationSummary>(SnapshotCategory,
		PlayerScopedKey(FString::Printf(TEXT("summary_l%d"), Limit)),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockNotificationSummary>)> OnAttempt)
		{
			return ClientRef->Get<FFlockNotificationSummary>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch notification summary"),
		MoveTemp(OnComplete));
}

// ─────────────────────────────────── Writes ───────────────────────────────────

void FFlockNotificationProvider::MarkRead(const FString& NotificationId,
	TFunction<void(TFlockResult<FFlockNotification>)> OnComplete)
{
	if (!RequireSignedIn<FFlockNotification>(OnComplete))
	{
		return;
	}
	if (!RequireNotEmpty(NotificationId, TEXT("Notification Id"), OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::NotificationRead(NotificationId));

	// Never queued offline. A read-receipt replayed later marks messages the player never saw, which is
	// strictly worse than losing the receipt — so this fails when unreachable rather than deferring.
	Execute<FFlockNotification>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockNotification>)> OnAttempt)
		{
			// Headers are read inside the operation so a refresh-and-replay carries the new bearer. The
			// session is captured as a shared ref, never `this` — teardown mid-flight must stay safe.
			return ClientRef->PostJson<FFlockNotification>(Url, SessionRef->GetAuthHeaders(), TEXT("{}"), MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete),
		TEXT("Mark notification read"));
}

void FFlockNotificationProvider::MarkAllRead(TFunction<void(TFlockResult<FFlockMarkAllReadResult>)> OnComplete)
{
	if (!RequireSignedIn<FFlockMarkAllReadResult>(OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::NotificationReadAll);

	Execute<FFlockMarkAllReadResult>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockMarkAllReadResult>)> OnAttempt)
		{
			return ClientRef->PostJson<FFlockMarkAllReadResult>(Url, SessionRef->GetAuthHeaders(), TEXT("{}"), MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete),
		TEXT("Mark all notifications read"));
}

void FFlockNotificationProvider::ClearCache()
{
	// Writes deliberately leave the snapshot alone — the next successful read overwrites it, and an inbox
	// with a slightly stale read flag beats an empty one on a plane. This is the logout path, where the
	// rows must go because they belong to the player who just left.
	DeleteSnapshotCategory(SnapshotCategory);
}
