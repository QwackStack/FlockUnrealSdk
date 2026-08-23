// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockNotificationProvider.h"

#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	/** Condensed JSON, the wire convention everywhere in this SDK. */
	FString SerializeObject(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}
}

const TCHAR* const FFlockNotificationProvider::SnapshotCategory = TEXT("notification");
const TCHAR* const FFlockNotificationProvider::StateSnapshotCategory = TEXT("notification_state");
const TCHAR* const FFlockNotificationProvider::TemplateSnapshotCategory = TEXT("notification_template");
const TCHAR* const FFlockNotificationProvider::WatermarkKey = TEXT("seen_watermark");
const TCHAR* const FFlockNotificationProvider::PendingSchedulesKey = TEXT("pending_schedules");

FFlockNotificationProvider::FFlockNotificationProvider(const TSharedRef<FFlockHttpClient>& InClient,
	const FFlockRetryPolicy& InPolicy, const TSharedRef<IFlockLogger>& InLogger,
	const TSharedRef<FFlockAuthSession>& InSession, const TWeakObjectPtr<UFlockEvents>& InEvents,
	const FString& InVersionedApiUrl,
	const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore, const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, Events(InEvents)
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

FString FFlockNotificationProvider::PlayerCacheScope() const
{
	const FString PlayerId = Session->GetPlayerId();
	return PlayerId.IsEmpty() ? FString() : FString::Printf(TEXT("%s/%s"), *GetSnapshotScope(SnapshotCategory), *PlayerId);
}

FString FFlockNotificationProvider::PlayerStateScope() const
{
	const FString PlayerId = Session->GetPlayerId();
	return PlayerId.IsEmpty() ? FString() : FString::Printf(TEXT("%s/%s"), *GetSnapshotScope(StateSnapshotCategory), *PlayerId);
}

bool FFlockNotificationProvider::TryMigrateLegacyState(const FString& Key, FString& OutPayload) const
{
	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	const FString PlayerId = Session->GetPlayerId();
	if (!Store.IsValid() || PlayerId.IsEmpty())
	{
		return false;
	}

	// Where an earlier version put it: the shared cache category, with the player id suffixed onto the key.
	const FString LegacyScope = GetSnapshotScope(SnapshotCategory);
	const FString LegacyKey = FString::Printf(TEXT("%s_%s"), *Key, *PlayerId);
	if (!Store->TryRead(LegacyScope, LegacyKey, OutPayload))
	{
		return false;
	}

	// Read the new location back before dropping the old one. FFlockSnapshotStore::Write returns void and
	// has three exits that only log — a failed serialize, a failed temp write, a failed move — so "it wrote"
	// cannot be assumed. Deleting on an unverified write is a one-way door: the pending-schedule list would
	// exist only in OutPayload for the rest of this call, and the next launch would read an empty list while
	// the server-side reminders still fire with their ids gone for good. That is precisely the harm this
	// whole change exists to prevent, arriving through its own migration.
	//
	// Keeping the legacy copy costs nothing on a retry: the precedence check above always prefers the new
	// location, so a later successful migration still wins and this simply runs again.
	Store->Write(PlayerStateScope(), Key, OutPayload);
	FString Verify;
	if (!Store->TryRead(PlayerStateScope(), Key, Verify))
	{
		Logger->LogWarning(FString::Printf(
			TEXT("Notification state '%s' could not be migrated (the new copy did not read back); keeping the ")
			TEXT("previous copy so nothing is lost. It will be retried on the next read."), *Key));
		return true;
	}
	Store->DeleteKey(LegacyScope, LegacyKey);
	return true;
}

// Pending-schedule bookkeeping

bool FFlockNotificationProvider::HasElapsed(const FString& DeliverAt)
{
	FDateTime Parsed;
	if (!FDateTime::ParseIso8601(*DeliverAt, Parsed))
	{
		// Kept, not dropped. This is the only handle on a cancellable reminder, so carrying a stale row
		// costs far less than losing the ability to cancel one.
		return false;
	}
	return Parsed <= FDateTime::UtcNow();
}

TArray<FFlockPendingSchedule> FFlockNotificationProvider::LoadPendingSchedules() const
{
	TArray<FFlockPendingSchedule> Stored;
	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	const FString Scope = PlayerStateScope();
	FString Payload;
	if (!Store.IsValid() || Scope.IsEmpty())
	{
		return Stored;
	}
	if (!Store->TryRead(Scope, PendingSchedulesKey, Payload) && !TryMigrateLegacyState(PendingSchedulesKey, Payload))
	{
		return Stored;
	}
	FFlockJsonUtils::ArrayFromPlainJson<FFlockPendingSchedule>(Payload, Stored);

	// Delivery can only be inferred from the clock — there is nothing to query — so an entry whose time has
	// passed stops being pending.
	TArray<FFlockPendingSchedule> Live;
	Live.Reserve(Stored.Num());
	for (const FFlockPendingSchedule& Entry : Stored)
	{
		if (!Entry.Id.IsEmpty() && !HasElapsed(Entry.DeliverAt))
		{
			Live.Add(Entry);
		}
	}

	// Rewritten only when something was actually dropped, so a plain read is not a write.
	if (Live.Num() != Stored.Num())
	{
		SavePendingSchedules(Live);
	}
	return Live;
}

void FFlockNotificationProvider::SavePendingSchedules(const TArray<FFlockPendingSchedule>& Pending) const
{
	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	const FString Scope = PlayerStateScope();
	if (!Store.IsValid() || Scope.IsEmpty())
	{
		// Cache disabled, or nobody signed in: scheduling still works, the game just has to keep the ids.
		return;
	}
	FString Payload;
	if (FFlockJsonUtils::ArrayToPlainJson<FFlockPendingSchedule>(Pending, Payload))
	{
		Store->Write(Scope, PendingSchedulesKey, Payload);
	}
}

void FFlockNotificationProvider::TrackPending(const FFlockScheduledNotification& Scheduled,
	const FString& TemplateName, const FString& TemplateId) const
{
	if (Scheduled.Id.IsEmpty())
	{
		// No id means nothing to cancel later, so there is nothing worth remembering.
		return;
	}

	TArray<FFlockPendingSchedule> Pending = LoadPendingSchedules();
	FFlockPendingSchedule Entry;
	Entry.Id = Scheduled.Id;
	Entry.TemplateName = TemplateName;
	Entry.TemplateId = TemplateId;
	// The server's echoed deliver_at, not the caller's requested one: the server is what decides when this
	// fires, and storing the request would let the two drift.
	Entry.DeliverAt = Scheduled.DeliverAt;
	Pending.Add(Entry);
	SavePendingSchedules(Pending);
}

void FFlockNotificationProvider::UntrackPending(const FString& ScheduledId) const
{
	TArray<FFlockPendingSchedule> Pending = LoadPendingSchedules();
	const int32 Removed = Pending.RemoveAll([&ScheduledId](const FFlockPendingSchedule& Entry)
	{
		return Entry.Id == ScheduledId;
	});
	if (Removed > 0)
	{
		SavePendingSchedules(Pending);
	}
}

// Seen-watermark bookkeeping

FFlockNotificationWatermark FFlockNotificationProvider::LoadWatermark() const
{
	FFlockNotificationWatermark Mark;
	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	const FString Scope = PlayerStateScope();
	if (!Store.IsValid() || Scope.IsEmpty())
	{
		return Mark;
	}
	FString Payload;
	if (Store->TryRead(Scope, WatermarkKey, Payload) || TryMigrateLegacyState(WatermarkKey, Payload))
	{
		// A corrupt or older-shaped record reads as unseeded, which costs one silent seed rather than a
		// burst of stale events.
		FFlockJsonUtils::PlainJsonToStruct<FFlockNotificationWatermark>(Payload, Mark);
	}
	return Mark;
}

void FFlockNotificationProvider::SaveWatermark(const FFlockNotificationWatermark& Mark) const
{
	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	const FString Scope = PlayerStateScope();
	if (!Store.IsValid() || Scope.IsEmpty())
	{
		// Cache disabled, or nobody signed in: the events still fire this run, they just cannot survive a
		// restart.
		return;
	}
	FString Payload;
	if (FFlockJsonUtils::StructToPlainJson<FFlockNotificationWatermark>(Mark, Payload))
	{
		Store->Write(Scope, WatermarkKey, Payload);
	}
}

void FFlockNotificationProvider::RaiseNewNotifications(const TArray<FFlockNotification>& Items) const
{
	UFlockEvents* Hub = Events.Get();
	if (!Hub)
	{
		// No hub (a provider built outside the subsystem, or teardown): skip the whole pass rather than
		// advancing the watermark past notifications nobody was told about.
		return;
	}

	FFlockNotificationWatermark Mark = LoadWatermark();
	const bool bFirstEver = !Mark.Seeded;

	FDateTime Cutoff = FDateTime::MinValue();
	if (!bFirstEver && !Mark.NewestCreatedAt.IsEmpty())
	{
		FDateTime::ParseIso8601(*Mark.NewestCreatedAt, Cutoff);
	}

	FDateTime Newest = Cutoff;
	TArray<FFlockNotification> Fresh;
	for (const FFlockNotification& Entry : Items)
	{
		FDateTime Created;
		// An unparseable created_at is skipped rather than announced: a duplicate event is worse than a miss.
		if (!FDateTime::ParseIso8601(*Entry.CreatedAt, Created))
		{
			continue;
		}
		if (Created > Newest)
		{
			Newest = Created;
		}
		if (!bFirstEver && Created > Cutoff)
		{
			Fresh.Add(Entry);
		}
	}

	// Persisted *before* anything is raised, so a handler that throws or re-enters cannot make the same
	// notification fire twice.
	if (bFirstEver || Newest > Cutoff)
	{
		Mark.Seeded = true;
		if (Newest > FDateTime::MinValue())
		{
			Mark.NewestCreatedAt = Newest.ToIso8601();
		}
		SaveWatermark(Mark);
	}

	// The page arrives newest-first, so walk it backwards and hand the game its mail in creation order.
	for (int32 Index = Fresh.Num() - 1; Index >= 0; --Index)
	{
		Hub->InvokeNotificationReceived(Fresh[Index]);
	}
}

void FFlockNotificationProvider::RaiseUnreadCount(int32 Count) const
{
	if (UFlockEvents* Hub = Events.Get())
	{
		Hub->InvokeUnreadCountChanged(Count);
	}
}

// Reads

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
	// The player is in the scope, not the key — see PlayerCacheScope(). RequireSignedIn above guarantees it
	// is non-empty by the time we get here.
	const FString Key = FString::Printf(TEXT("inbox_p%d_l%d_u%d"), Page, Limit, bUnreadOnly ? 1 : 0);
	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();

	FetchAtScope<FFlockNotificationPage>(PlayerCacheScope(), Key,
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
		[WeakSelf, OnComplete](TFlockResult<FFlockNotificationPage> Result)
		{
			// Raised before the caller's completion so a handler that repaints an inbox badge sees the
			// same state the caller is about to act on. A cache-served page runs through this too: the
			// watermark is what decides novelty, not where the rows came from.
			if (Result.bSuccess)
			{
				if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
				{
					Self->RaiseNewNotifications(Result.Value.Items);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
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
	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();

	FetchAtScope<FFlockUnreadCount>(PlayerCacheScope(), TEXT("unread_count"),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockUnreadCount>)> OnAttempt)
		{
			// Enveloped ({error,response,result}) — the enveloped verb unwraps `result`.
			return ClientRef->Get<FFlockUnreadCount>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch unread count"),
		[WeakSelf, OnComplete](TFlockResult<FFlockUnreadCount> Result)
		{
			if (Result.bSuccess)
			{
				if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
				{
					Self->RaiseUnreadCount(Result.Value.Count);
				}
			}
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
	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();

	FetchAtScope<FFlockNotificationSummary>(PlayerCacheScope(),
		FString::Printf(TEXT("summary_l%d"), Limit),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockNotificationSummary>)> OnAttempt)
		{
			return ClientRef->Get<FFlockNotificationSummary>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch notification summary"),
		[WeakSelf, OnComplete](TFlockResult<FFlockNotificationSummary> Result)
		{
			// The one call that serves both events: it reports a count *and* carries rows.
			if (Result.bSuccess)
			{
				if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
				{
					Self->RaiseUnreadCount(Result.Value.UnreadCount);
					Self->RaiseNewNotifications(Result.Value.Items);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

// Writes

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
	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();

	Execute<FFlockMarkAllReadResult>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockMarkAllReadResult>)> OnAttempt)
		{
			return ClientRef->PostJson<FFlockMarkAllReadResult>(Url, SessionRef->GetAuthHeaders(), TEXT("{}"), MoveTemp(OnAttempt));
		},
		[WeakSelf, OnComplete](TFlockResult<FFlockMarkAllReadResult> Result)
		{
			// The server does not echo a count here, but a successful mark-all-read has exactly one
			// possible outcome — nothing is unread — so zero is reported rather than left to a refetch.
			// Raised even when Updated is 0: a badge that was already clear stays clear either way.
			if (Result.bSuccess)
			{
				if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
				{
					Self->RaiseUnreadCount(0);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Mark all notifications read"));
}

// Template catalog

void FFlockNotificationProvider::GetTemplates(TFunction<void(TFlockResult<TArray<FFlockNotificationTemplate>>)> OnComplete)
{
	// No sign-in gate: this route declares no Authorization header and answers a game-scoped schema. Gating
	// it would refuse a perfectly valid call — a title screen may want the catalog before anyone signs in.
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::NotificationTemplates);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();

	// Keyed by game version rather than player, unlike every other key in this provider — the catalog
	// belongs to the game, so it must survive a sign-out and must not be player-suffixed.
	FetchListWithSnapshot<FFlockNotificationTemplate>(TemplateSnapshotCategory, TEXT("templates"),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockNotificationTemplate>>)> OnAttempt)
		{
			// Enveloped **list**: `result` is a bare array, not the {items,total,page,limit} page shape.
			return ClientRef->GetList<FFlockNotificationTemplate>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch notification templates"),
		[WeakSelf, OnComplete](TFlockResult<TArray<FFlockNotificationTemplate>> Result)
		{
			if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					// Warm the name memo from the catalog, so a later send by name costs no round trip.
					for (const FFlockNotificationTemplate& Template : Result.Value)
					{
						if (!Template.Name.IsEmpty())
						{
							Self->TemplatesByName.Add(Template.Name, Template);
						}
					}
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockNotificationProvider::GetTemplateByName(const FString& TemplateName, const FString& Locale,
	TFunction<void(TFlockResult<FFlockNotificationTemplate>)> OnComplete)
{
	if (!RequireNotEmpty(TemplateName, TEXT("Template Name"), OnComplete))
	{
		return;
	}
	// A locale-specific fetch is not the same record, so only the default-locale lookup is memoized.
	if (Locale.IsEmpty())
	{
		if (const FFlockNotificationTemplate* Memoized = TemplatesByName.Find(TemplateName))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockNotificationTemplate>::Ok(*Memoized));
			}
			return;
		}
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::NotificationTemplateByName(TemplateName, Locale));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockNotificationTemplate>(TemplateSnapshotCategory,
		FString::Printf(TEXT("template_%s%s"), *TemplateName, *(Locale.IsEmpty() ? FString() : TEXT("_") + Locale)),
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockNotificationTemplate>)> OnAttempt)
		{
			return ClientRef->Get<FFlockNotificationTemplate>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch notification template"),
		[WeakSelf, TemplateName, Locale, OnComplete](TFlockResult<FFlockNotificationTemplate> Result)
		{
			if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess && Locale.IsEmpty() && !Result.Value.Id.IsEmpty())
				{
					Self->TemplatesByName.Add(TemplateName, Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		});
}

void FFlockNotificationProvider::WithTemplateId(const FString& TemplateName, TFunction<void(const FString&)> Continue,
	TFunction<void(const FFlockError&)> OnFailure)
{
	if (const FFlockNotificationTemplate* Memoized = TemplatesByName.Find(TemplateName))
	{
		Continue(Memoized->Id);
		return;
	}

	GetTemplateByName(TemplateName, FString(),
		[Continue, OnFailure, TemplateName](TFlockResult<FFlockNotificationTemplate> Result)
		{
			if (!Result.bSuccess)
			{
				OnFailure(Result.Error);
				return;
			}
			if (Result.Value.Id.IsEmpty())
			{
				// A 2xx with no id means the name does not exist for this game. That is a caller mistake,
				// so it fails Validation rather than silently scheduling against nothing.
				OnFailure(FFlockError::Make(EFlockErrorType::Validation,
					FString::Printf(TEXT("No notification template named '%s'"), *TemplateName)));
				return;
			}
			Continue(Result.Value.Id);
		});
}

// Scheduling

void FFlockNotificationProvider::ScheduleByTemplateName(const FString& TemplateName, const FDateTime& DeliverAtUtc,
	const FFlockCommandData& Variables, const TArray<EFlockNotificationChannel>& Channels,
	TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete)
{
	if (!RequireSignedIn<FFlockScheduledNotification>(OnComplete))
	{
		return;
	}
	if (!RequireNotEmpty(TemplateName, TEXT("Template Name"), OnComplete))
	{
		return;
	}

	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();
	const FDateTime When = DeliverAtUtc;
	const FFlockCommandData Vars = Variables;
	const TArray<EFlockNotificationChannel> Chans = Channels;

	const FString Name = TemplateName;

	WithTemplateId(TemplateName,
		[WeakSelf, Name, When, Vars, Chans, OnComplete](const FString& TemplateId)
		{
			if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
			{
				// Straight to the internal path, carrying the name — that is the only thing this entry
				// point knows that the by-id one does not, and it is what makes a tracked entry readable.
				Self->ScheduleInternal(TemplateId, Name, When, Vars, Chans, OnComplete);
			}
		},
		[OnComplete](const FFlockError& Error)
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockScheduledNotification>::Fail(Error));
			}
		});
}

void FFlockNotificationProvider::ScheduleByTemplateId(const FString& TemplateId, const FDateTime& DeliverAtUtc,
	const FFlockCommandData& Variables, const TArray<EFlockNotificationChannel>& Channels,
	TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete)
{
	// No name to record: a caller who already holds an id never went through the catalog.
	ScheduleInternal(TemplateId, FString(), DeliverAtUtc, Variables, Channels, MoveTemp(OnComplete));
}

void FFlockNotificationProvider::ScheduleInternal(const FString& TemplateId, const FString& TemplateName,
	const FDateTime& DeliverAtUtc,
	const FFlockCommandData& Variables, const TArray<EFlockNotificationChannel>& Channels,
	TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete)
{
	if (!RequireSignedIn<FFlockScheduledNotification>(OnComplete))
	{
		return;
	}
	if (!RequireNotEmpty(TemplateId, TEXT("Template Id"), OnComplete))
	{
		return;
	}

	// The body is assembled as an FJsonObject rather than exported from a struct: `variables` is free-form,
	// which the reflection path cannot express, and the optional members must be omitted rather than sent
	// empty. Same reasoning as the command bodies.
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("template_id"), TemplateId);
	// ISO-8601 UTC. The caller hands over an FDateTime so a hand-built string cannot get the format wrong.
	Body->SetStringField(TEXT("deliver_at"), DeliverAtUtc.ToIso8601());

	if (!Variables.IsEmpty())
	{
		Body->SetObjectField(TEXT("variables"), Variables.ToJsonObject());
	}
	if (Channels.Num() > 0)
	{
		// Omitted entirely when empty, so the server applies the template's own channel defaults rather
		// than receiving an empty list that reads as "deliver nowhere".
		TArray<TSharedPtr<FJsonValue>> Wire;
		Wire.Reserve(Channels.Num());
		for (const EFlockNotificationChannel Channel : Channels)
		{
			Wire.Add(MakeShared<FJsonValueString>(FlockNotificationChannelToWire(Channel)));
		}
		Body->SetArrayField(TEXT("channels"), Wire);
	}

	const FString Json = SerializeObject(Body);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::NotificationSchedule);

	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();
	const FString TrackedName = TemplateName;
	const FString TrackedId = TemplateId;

	Execute<FFlockScheduledNotification>(
		[ClientRef, SessionRef, Url, Json](TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnAttempt)
		{
			return ClientRef->PostJson<FFlockScheduledNotification>(Url, SessionRef->GetAuthHeaders(), Json, MoveTemp(OnAttempt));
		},
		[WeakSelf, TrackedName, TrackedId, OnComplete](TFlockResult<FFlockScheduledNotification> Result)
		{
			// Tracked before the caller is told, so a handler that immediately reads GetPendingSchedules()
			// sees the entry it just created.
			if (Result.bSuccess)
			{
				if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
				{
					Self->TrackPending(Result.Value, TrackedName, TrackedId);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Schedule notification"),
		// Not idempotent: a replay after an ambiguous failure would leave the player with two of the same
		// reminder. Failing once and letting the caller decide beats silently double-scheduling.
		/*bIdempotent*/ false);
}

void FFlockNotificationProvider::CancelScheduled(const FString& ScheduledId,
	TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnComplete)
{
	if (!RequireSignedIn<FFlockScheduledNotification>(OnComplete))
	{
		return;
	}
	if (!RequireNotEmpty(ScheduledId, TEXT("Scheduled Id"), OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::NotificationScheduleById(ScheduledId));

	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();
	const FString CancelledId = ScheduledId;

	// Idempotent, unlike the schedule call: cancelling twice lands on the same state, so a retry after an
	// ambiguous failure is safe and is what the caller wants.
	Execute<FFlockScheduledNotification>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnAttempt)
		{
			return ClientRef->Delete<FFlockScheduledNotification>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		[WeakSelf, CancelledId, OnComplete](TFlockResult<FFlockScheduledNotification> Result)
		{
			if (Result.bSuccess)
			{
				if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
				{
					Self->UntrackPending(CancelledId);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Cancel scheduled notification"));
}

TArray<FFlockPendingSchedule> FFlockNotificationProvider::GetPendingSchedules() const
{
	// Deliberately not sign-in gated in the way the network calls are: there is no request to make, and the
	// signed out there is no state scope at all, so LoadPendingSchedules bails out and this reads empty.
	return LoadPendingSchedules();
}

void FFlockNotificationProvider::CancelAllScheduled(TFunction<void(TFlockResult<int32>)> OnComplete)
{
	if (!RequireSignedIn<int32>(OnComplete))
	{
		return;
	}

	const TArray<FFlockPendingSchedule> Pending = LoadPendingSchedules();
	if (Pending.Num() == 0)
	{
		// Nothing tracked is a success with zero cancelled, not a failure — the caller asked for an end
		// state, and it already holds.
		if (OnComplete)
		{
			OnComplete(TFlockResult<int32>::Ok(0));
		}
		return;
	}

	// The ids are snapshotted up front: every cancel rewrites the stored list, so walking the live list
	// while mutating it would skip entries.
	TSharedRef<TArray<FString>> Ids = MakeShared<TArray<FString>>();
	Ids->Reserve(Pending.Num());
	for (const FFlockPendingSchedule& Entry : Pending)
	{
		Ids->Add(Entry.Id);
	}

	CancelAllStep(Ids, 0, MakeShared<int32>(0), MoveTemp(OnComplete));
}

void FFlockNotificationProvider::CancelAllStep(TSharedRef<TArray<FString>> Ids, int32 Index,
	TSharedRef<int32> Cancelled, TFunction<void(TFlockResult<int32>)> OnComplete)
{
	if (!Ids->IsValidIndex(Index))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<int32>::Ok(*Cancelled));
		}
		return;
	}

	TWeakPtr<FFlockNotificationProvider> WeakSelf = AsShared();
	const FString Id = (*Ids)[Index];

	// Sequential rather than parallel: each cancel rewrites the persisted list, so overlapping writes would
	// race and the last one home would resurrect entries the others had removed.
	CancelScheduled(Id, [WeakSelf, Ids, Index, Cancelled, Id, OnComplete](TFlockResult<FFlockScheduledNotification> Result)
	{
		const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}

		if (Result.bSuccess)
		{
			++(*Cancelled);
		}
		else if (FFlockError::IsPermanentStatus(Result.Error.StatusCode))
		{
			// Delivered, already cancelled, or unknown to the server — it is not pending either way, so
			// drop it and keep going rather than failing the batch on an entry nothing can act on.
			Self->UntrackPending(Id);
		}
		else
		{
			// Transient: stop here and surface it. The remaining entries stay tracked, so a later call
			// picks up where this one left off instead of losing them.
			if (OnComplete)
			{
				OnComplete(TFlockResult<int32>::Fail(Result.Error));
			}
			return;
		}

		Self->CancelAllStep(Ids, Index + 1, Cancelled, OnComplete);
	});
}

// Push device tokens

bool FFlockNotificationProvider::GetCurrentDevicePlatform(EFlockDevicePlatform& OutPlatform)
{
	return FlockTryResolveDevicePlatform(FString(ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName())), OutPlatform);
}

void FFlockNotificationProvider::RegisterDeviceToken(EFlockDevicePlatform Platform, const FString& Token,
	TFunction<void(TFlockResult<FFlockDeviceToken>)> OnComplete)
{
	if (!RequireSignedIn<FFlockDeviceToken>(OnComplete))
	{
		return;
	}
	if (!RequireNotEmpty(Token, TEXT("Device Token"), OnComplete))
	{
		return;
	}

	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("platform"), FlockDevicePlatformToWire(Platform));
	Body->SetStringField(TEXT("token"), Token);
	const FString Json = SerializeObject(Body);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::DeviceTokenRegister);

	// Idempotent, unlike scheduling: the row is keyed by token, so re-sending after an ambiguous failure
	// lands on the same state rather than creating a second registration.
	Execute<FFlockDeviceToken>(
		[ClientRef, SessionRef, Url, Json](TFunction<void(TFlockResult<FFlockDeviceToken>)> OnAttempt)
		{
			return ClientRef->PostJson<FFlockDeviceToken>(Url, SessionRef->GetAuthHeaders(), Json, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete),
		TEXT("Register device token"));
}

void FFlockNotificationProvider::RegisterDeviceToken(const FString& Token,
	TFunction<void(TFlockResult<FFlockDeviceToken>)> OnComplete)
{
	EFlockDevicePlatform Platform = EFlockDevicePlatform::Android;
	if (!GetCurrentDevicePlatform(Platform))
	{
		// Refused rather than guessed. The backend takes android/ios/web only, and a token filed under the
		// wrong platform is accepted here and then never delivers - a failure that surfaces weeks later as
		// "push is broken" with nothing in the logs to point at.
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockDeviceToken>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				FString::Printf(TEXT("Push notifications are not available on %s. The backend accepts android, ios and web only."),
					ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName())))));
		}
		return;
	}
	RegisterDeviceToken(Platform, Token, MoveTemp(OnComplete));
}

void FFlockNotificationProvider::UnregisterDeviceToken(const FString& Token,
	TFunction<void(TFlockResult<FFlockUnregisterDeviceTokenResult>)> OnComplete)
{
	if (!RequireSignedIn<FFlockUnregisterDeviceTokenResult>(OnComplete))
	{
		return;
	}
	if (!RequireNotEmpty(Token, TEXT("Device Token"), OnComplete))
	{
		return;
	}

	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("token"), Token);
	const FString Json = SerializeObject(Body);

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::DeviceTokenUnregister);

	Execute<FFlockUnregisterDeviceTokenResult>(
		[ClientRef, SessionRef, Url, Json](TFunction<void(TFlockResult<FFlockUnregisterDeviceTokenResult>)> OnAttempt)
		{
			return ClientRef->PostJson<FFlockUnregisterDeviceTokenResult>(Url, SessionRef->GetAuthHeaders(), Json, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete),
		TEXT("Unregister device token"));
}

void FFlockNotificationProvider::ClearCache()
{
	// Writes deliberately leave the snapshot alone — the next successful read overwrites it, and an inbox
	// with a slightly stale read flag beats an empty one on a plane. This is the logout path, where the
	// rows must go because they belong to the player who just left.
	//
	// **Still call this while the departing player's session is live.** The read-restore dance is gone, but
	// the precondition is not — it inverted. It used to be "clear before the tokens go, or the watermark is
	// lost"; it is now "clear before the tokens go, or nothing is cleared at all". With no player id there is
	// no scope to delete, so a reordered Logout() would leave the departing player's whole inbox on disk, and
	// **every existing test would still pass** (they call this directly while signed in, and the signed-out
	// no-op is itself pinned as correct). Hence the warning below rather than a silent return.
	//
	// One scope, one player. Everything this deletes belongs to the player signing out; everything that
	// must survive is somewhere else already:
	//
	//   - another player's inbox lives under their own scope — a shared device is the case this exists for;
	//   - the seen-watermark and the pending-schedule list are state, and live in StateSnapshotCategory;
	//   - the template catalog and its name memo are game-scoped, and live in TemplateSnapshotCategory.
	//
	// None of that is a rule this function enforces — it is where the data is, which is why there is no
	// read-then-restore here any more, and why the ordering inside Logout() no longer matters.
	const TSharedPtr<FFlockSnapshotStore> Store = GetSnapshotStore();
	const FString Scope = PlayerCacheScope();
	if (!Store.IsValid())
	{
		return;
	}
	if (Scope.IsEmpty())
	{
		// Nobody signed in: there is no cache to clear, and no scope that could safely stand in for one (an
		// empty segment collapses to the bare category and takes every account on the device). Logged rather
		// than returned silently, because the other way to reach this is a Logout() that cleared the tokens
		// first — in which case a player's inbox is being left behind and nothing else would say so.
		Logger->LogWarning(TEXT("Notification ClearCache called with no player signed in; nothing was cleared. ")
			TEXT("If this ran during logout, the cache must be cleared before the session tokens are."));
		return;
	}

	// A **one-upgrade residue, deliberately accepted.** Inbox snapshots written by 1.3.0-1.6.0 sit directly
	// in the category rather than in this player's subdirectory, and they are not removed here. There is no
	// safe way to: the delete is recursive (it would take the new per-player subdirectories with it), another
	// player's not-yet-migrated state still lives in that flat directory, and a filename filter is
	// untrustworthy because Sanitize caps the readable prefix at 64 chars. They are inert — nothing writes
	// there any more — and PruneOtherVersions culls the whole tree on a game-version change.
	Store->DeleteScope(Scope);
}
