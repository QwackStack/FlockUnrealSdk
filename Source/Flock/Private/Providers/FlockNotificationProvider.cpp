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
const TCHAR* const FFlockNotificationProvider::TemplateSnapshotCategory = TEXT("notification_template");

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

	Execute<FFlockMarkAllReadResult>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockMarkAllReadResult>)> OnAttempt)
		{
			return ClientRef->PostJson<FFlockMarkAllReadResult>(Url, SessionRef->GetAuthHeaders(), TEXT("{}"), MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete),
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

	WithTemplateId(TemplateName,
		[WeakSelf, When, Vars, Chans, OnComplete](const FString& TemplateId)
		{
			if (const TSharedPtr<FFlockNotificationProvider> Self = WeakSelf.Pin())
			{
				Self->ScheduleByTemplateId(TemplateId, When, Vars, Chans, OnComplete);
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

	Execute<FFlockScheduledNotification>(
		[ClientRef, SessionRef, Url, Json](TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnAttempt)
		{
			return ClientRef->PostJson<FFlockScheduledNotification>(Url, SessionRef->GetAuthHeaders(), Json, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete),
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

	// Idempotent, unlike the schedule call: cancelling twice lands on the same state, so a retry after an
	// ambiguous failure is safe and is what the caller wants.
	Execute<FFlockScheduledNotification>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockScheduledNotification>)> OnAttempt)
		{
			return ClientRef->Delete<FFlockScheduledNotification>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete),
		TEXT("Cancel scheduled notification"));
}

void FFlockNotificationProvider::ClearCache()
{
	// Writes deliberately leave the snapshot alone — the next successful read overwrites it, and an inbox
	// with a slightly stale read flag beats an empty one on a plane. This is the logout path, where the
	// rows must go because they belong to the player who just left.
	DeleteSnapshotCategory(SnapshotCategory);

	// The template catalog and its name memo are **kept**: both are game-scoped, so nothing in them belongs
	// to the departing player. Dropping them would make a title screen refetch the catalog after every
	// sign-out for no benefit. This is why templates get their own snapshot category.
}
