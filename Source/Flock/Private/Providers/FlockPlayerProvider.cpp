// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockPlayerProvider.h"
#include "Http/FlockErrorHints.h"

#include "Http/FlockEndpoints.h"

const TCHAR* const FFlockPlayerProvider::SnapshotCategory = TEXT("player_template");

FFlockPlayerProvider::FFlockPlayerProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
	const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	// A call whose token has expired renews it once and is sent again, as on every provider that acts for the player.
	SetAuthSession(InSession);
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
}

// ───────────────────────────────── Player data ─────────────────────────────────

void FFlockPlayerProvider::GetDataById(const FString& PlayerDataId, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(PlayerDataId, TEXT("Player Data ID"), OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(FlockEndpoints::PlayerDataById(PlayerDataId));

	// Enveloped ({error,response,result}); never cached — a direct row read is always fresh. Sent only while the sign-in
	// current now lasts, the token read on each attempt.
	Execute<FFlockPlayerData>(ForTheCurrentSignInOnly<FFlockPlayerData>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockPlayerData>)> OnAttempt)
		{
			return ClientRef->Get<FFlockPlayerData>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		}),
		MoveTemp(OnComplete), TEXT("Fetch player data"));
}

void FFlockPlayerProvider::GetAllData(const FString& PlayerId, int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnComplete)
{
	ReadDataPage(PlayerId, Session->GetSignInNumber(), Page, Limit, MoveTemp(OnComplete));
}

void FFlockPlayerProvider::ReadDataPage(const FString& PlayerId, int32 SignInNumber, int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnComplete)
{
	FString Path = FString::Printf(TEXT("%s?page=%d&limit=%d"), FlockEndpoints::PlayerData, Page, Limit);
	if (!PlayerId.IsEmpty())
	{
		Path += FString::Printf(TEXT("&player_id=%s"), *PlayerId);
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = MakeUrl(Path);

	// Bare paginated ({items,total,page,limit}); never cached. GetPaged returns the template page — copy it
	// into the concrete, Blueprint-able page struct. Sent only while SignInNumber lasts, the token read on each attempt.
	Execute<FFlockPlayerDataPage>(ForSignInOnly<FFlockPlayerDataPage>(SessionRef, SignInNumber,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnAttempt)
		{
			return ClientRef->GetPaged<FFlockPlayerData>(Url, SessionRef->GetAuthHeaders(),
				[OnAttempt](TFlockResult<TFlockPage<FFlockPlayerData>> Result)
				{
					if (!Result.bSuccess)
					{
						OnAttempt(TFlockResult<FFlockPlayerDataPage>::Fail(Result.Error));
						return;
					}
					FFlockPlayerDataPage Page;
					Page.Items = Result.Value.Items;
					Page.Total = Result.Value.Total;
					Page.Page = Result.Value.Page;
					Page.Limit = Result.Value.Limit;
					OnAttempt(TFlockResult<FFlockPlayerDataPage>::Ok(Page));
				});
		}),
		MoveTemp(OnComplete), TEXT("Fetch player data list"));
}

void FFlockPlayerProvider::GetMyData(int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnComplete)
{
	// Resolved here rather than passed through empty, because empty means *all players* on the underlying
	// call — the opposite of what this overload promises. Signed out is a Validation failure, not a
	// silent whole-game read.
	const FString PlayerId = Session->GetPlayerId();
	if (!RequireNotEmpty(PlayerId, TEXT("Signed-in player's id"), OnComplete, FFlockErrorHints::SignInFirst()))
	{
		return;
	}
	GetAllData(PlayerId, Page, Limit, MoveTemp(OnComplete));
}

void FFlockPlayerProvider::GetMyDataByTemplate(const FString& PlayerTemplateId, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(PlayerTemplateId, TEXT("Player Template ID"), OnComplete))
	{
		return;
	}
	const FString PlayerId = Session->GetPlayerId();
	if (!RequireNotEmpty(PlayerId, TEXT("Signed-in player's id"), OnComplete, FFlockErrorHints::SignInFirst()))
	{
		return;
	}

	const FString TemplateId = PlayerTemplateId;
	GetOrFetchPlayerData(PlayerId,
		[TemplateId, OnComplete](TFlockResult<TMap<FString, FFlockPlayerData>> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			if (!Result.bSuccess)
			{
				OnComplete(TFlockResult<FFlockPlayerData>::Fail(Result.Error));
				return;
			}
			if (const FFlockPlayerData* Row = Result.Value.Find(TemplateId))
			{
				OnComplete(TFlockResult<FFlockPlayerData>::Ok(*Row));
				return;
			}
			// No row for this template yet — Ok with an empty record (empty Id) rather than a failure,
			// because not having written one is a normal state and not an error the caller can act on.
			OnComplete(TFlockResult<FFlockPlayerData>::Ok(FFlockPlayerData()));
		});
}

void FFlockPlayerProvider::GetMyDataByTag(const FString& Tag, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(Tag, TEXT("Template tag"), OnComplete))
	{
		return;
	}

	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();
	GetTemplateByTag(Tag,
		[WeakSelf, OnComplete](TFlockResult<FFlockPlayerTemplateSchema> TemplateResult)
		{
			const TSharedPtr<FFlockPlayerProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!TemplateResult.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerData>::Fail(TemplateResult.Error));
				}
				return;
			}
			Self->GetMyDataByTemplate(TemplateResult.Value.Id, OnComplete);
		});
}

void FFlockPlayerProvider::GetWallet(TFunction<void(TFlockResult<FFlockWallet>)> OnComplete)
{
	GetMyDataByTag(TEXT("currency"), [OnComplete](TFlockResult<FFlockPlayerData> Result)
	{
		if (!OnComplete)
		{
			return;
		}
		if (!Result.bSuccess)
		{
			OnComplete(TFlockResult<FFlockWallet>::Fail(Result.Error));
			return;
		}
		if (Result.Value.Id.IsEmpty())
		{
			OnComplete(TFlockResult<FFlockWallet>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("The signed-in player has no wallet in this Game Version"),
				TEXT("The server makes a player's wallet when the player registers, from the Game Version's currency template, so a player who registered before that template existed, or under another Game Version, has none. Create the currency template first, then register the player under this build's Game Version."))));
			return;
		}
		OnComplete(TFlockResult<FFlockWallet>::Ok(FFlockWallet::From(Result.Value)));
	});
}

// ─────────────────────────────────── Templates ─────────────────────────────────

void FFlockPlayerProvider::GetTemplates(TFunction<void(TFlockResult<TArray<FFlockPlayerTemplateSchema>>)> OnComplete)
{
	if (bAllTemplatesFetched)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockPlayerTemplateSchema>>::Ok(ResolveTemplates(AllTemplateIds)));
		}
		return;
	}

	const FString Key = TEXT("all");
	if (!TemplateListInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::PlayerTemplate);
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	// Enveloped list ({error,response,result:[...]}) — GetList unwraps `result` as a bare array.
	FetchListWithSnapshot<FFlockPlayerTemplateSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<TArray<FFlockPlayerTemplateSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockPlayerTemplateSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		TEXT("Fetch player templates"),
		[WeakSelf, Key](TFlockResult<TArray<FFlockPlayerTemplateSchema>> Result)
		{
			const TSharedPtr<FFlockPlayerProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				Self->AllTemplateIds.Reset();
				for (const FFlockPlayerTemplateSchema& Template : Result.Value)
				{
					Self->IndexTemplate(Template);
					if (!Template.Id.IsEmpty())
					{
						Self->AllTemplateIds.Add(Template.Id);
					}
				}
				Self->bAllTemplatesFetched = true;
			}
			Self->TemplateListInFlight.Complete(Key, Result);
		});
}

void FFlockPlayerProvider::GetTemplateById(const FString& PlayerTemplateId, TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnComplete)
{
	if (!RequireNotEmpty(PlayerTemplateId, TEXT("Player Template ID"), OnComplete))
	{
		return;
	}
	if (const FFlockPlayerTemplateSchema* Cached = TemplatesById.Find(PlayerTemplateId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPlayerTemplateSchema>::Ok(*Cached));
		}
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::PlayerTemplateById(PlayerTemplateId));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	// Memoize only: a single by-id template read is not worth a snapshot entry — the all-templates
	// fetch is the one that has to survive an outage.
	Execute<FFlockPlayerTemplateSchema>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockPlayerTemplateSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		[WeakSelf, OnComplete](TFlockResult<FFlockPlayerTemplateSchema> Result)
		{
			if (const TSharedPtr<FFlockPlayerProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->IndexTemplate(Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Fetch player template"));
}

void FFlockPlayerProvider::GetTemplateByName(const FString& Name, TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnComplete)
{
	if (!RequireNotEmpty(Name, TEXT("Player Template Name"), OnComplete))
	{
		return;
	}
	if (const FString* Id = TemplateIdByName.Find(Name))
	{
		if (const FFlockPlayerTemplateSchema* Cached = TemplatesById.Find(*Id))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockPlayerTemplateSchema>::Ok(*Cached));
			}
			return;
		}
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::PlayerTemplateByName(Name));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	Execute<FFlockPlayerTemplateSchema>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockPlayerTemplateSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		[WeakSelf, OnComplete](TFlockResult<FFlockPlayerTemplateSchema> Result)
		{
			if (const TSharedPtr<FFlockPlayerProvider> Self = WeakSelf.Pin())
			{
				if (Result.bSuccess)
				{
					Self->IndexTemplate(Result.Value);
				}
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Fetch player template by name"));
}

void FFlockPlayerProvider::GetTemplateByTag(const FString& Tag, TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnComplete)
{
	if (!RequireNotEmpty(Tag, TEXT("Template tag"), OnComplete))
	{
		return;
	}

	const FString TagCopy = Tag;
	GetTemplates(
		[TagCopy, OnComplete](TFlockResult<TArray<FFlockPlayerTemplateSchema>> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			if (!Result.bSuccess)
			{
				OnComplete(TFlockResult<FFlockPlayerTemplateSchema>::Fail(Result.Error));
				return;
			}
			for (const FFlockPlayerTemplateSchema& Template : Result.Value)
			{
				if (!Template.Id.IsEmpty() && Template.Tag.Equals(TagCopy, ESearchCase::IgnoreCase))
				{
					OnComplete(TFlockResult<FFlockPlayerTemplateSchema>::Ok(Template));
					return;
				}
			}
			OnComplete(TFlockResult<FFlockPlayerTemplateSchema>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
				FString::Printf(TEXT("No player template is tagged '%s' in this game version"), *TagCopy), TEXT("Tag a player-data template with it in the Flock dashboard, under the Game Version this build runs."))));
		});
}

void FFlockPlayerProvider::GetTemplatePlayerData(const FString& PlayerTemplateId, TFunction<void(TFlockResult<TArray<FFlockPlayerData>>)> OnComplete)
{
	if (!RequireNotEmpty(PlayerTemplateId, TEXT("Player Template ID"), OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::PlayerTemplateData(PlayerTemplateId));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;

	// Enveloped list; never cached (a template's rows change as players play).
	Execute<TArray<FFlockPlayerData>>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<TArray<FFlockPlayerData>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockPlayerData>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("Fetch player data for template"));
}

// ─────────────────────────────────────── Bans ──────────────────────────────────

void FFlockPlayerProvider::GetBan(const FString& PlayerId, TFunction<void(TFlockResult<FFlockPlayerBan>)> OnComplete)
{
	const FString ResolvedPlayerId = PlayerId.IsEmpty() ? Session->GetPlayerId() : PlayerId;
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID"), OnComplete, FFlockErrorHints::SignInFirst()))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s?player_id=%s"), FlockEndpoints::PlayerBan, *ResolvedPlayerId));
	const TSharedRef<FFlockAuthSession> SessionRef = Session;

	// Enveloped but nullable (Union[PlayerBanSchema, None]) — GetRaw into the wrapper, whose custom parse
	// treats a null `result` as "not banned". Never cached: a ban can change server-side at any time. Sent only while the
	// sign-in current now lasts.
	Execute<FFlockPlayerBanResponse>(ForTheCurrentSignInOnly<FFlockPlayerBanResponse>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockPlayerBanResponse>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockPlayerBanResponse>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		}),
		[OnComplete](TFlockResult<FFlockPlayerBanResponse> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			if (!Result.bSuccess)
			{
				OnComplete(TFlockResult<FFlockPlayerBan>::Fail(Result.Error));
				return;
			}
			OnComplete(TFlockResult<FFlockPlayerBan>::Ok(Result.Value.Ban));
		},
		TEXT("Get player ban"));
}

// ────────────────────────── Game-commands write-through seam ───────────────────

void FFlockPlayerProvider::ApplyServerPlayerData(const FFlockPlayerData& Row)
{
	if (Row.PlayerId.IsEmpty())
	{
		return;
	}
	// Newer than any read of player data already on its way: that read is not kept, and a read from now on is sent afresh.
	DropReadsOnTheirWay(Row.PlayerId);
	if (Row.Id.IsEmpty() || Row.PlayerTemplateId.IsEmpty())
	{
		return;
	}
	// Only fold into a map that already exists. Creating one here would mark the player as "fully cached"
	// off a single row, and every other template would then read as absent without a refetch.
	if (TMap<FString, FFlockPlayerData>* Rows = PlayerDataByPlayer.Find(Row.PlayerId))
	{
		// Two money answers can arrive in the opposite order to the one the server wrote them in: the older never replaces
		// the newer. A row whose time cannot be read replaces the cached one, as before.
		if (const FFlockPlayerData* Cached = Rows->Find(Row.PlayerTemplateId))
		{
			FDateTime CachedAt;
			FDateTime RowAt;
			if (FDateTime::ParseIso8601(*Cached->UpdatedAt, CachedAt) && FDateTime::ParseIso8601(*Row.UpdatedAt, RowAt) && RowAt < CachedAt)
			{
				return;
			}
		}
		// A refusal describes the one write it came back with, never the row a later read finds.
		FFlockPlayerData Kept = Row;
		Kept.LeaderboardRefusals.Reset();
		Rows->Add(Row.PlayerTemplateId, Kept);
	}
}

bool FFlockPlayerProvider::TryGetCachedRow(const FString& PlayerDataId, FFlockPlayerData& OutRow) const
{
	if (PlayerDataId.IsEmpty())
	{
		return false;
	}
	for (const TPair<FString, TMap<FString, FFlockPlayerData>>& Player : PlayerDataByPlayer)
	{
		for (const TPair<FString, FFlockPlayerData>& Entry : Player.Value)
		{
			if (Entry.Value.Id == PlayerDataId)
			{
				OutRow = Entry.Value;
				return true;
			}
		}
	}
	return false;
}

void FFlockPlayerProvider::EvictPlayerCacheByRow(const FString& PlayerDataId)
{
	if (PlayerDataId.IsEmpty())
	{
		return;
	}
	// Owner resolved first, removed after: dropping an entry mid-iteration would invalidate the walk.
	FString OwningPlayerId;
	for (const TPair<FString, TMap<FString, FFlockPlayerData>>& Player : PlayerDataByPlayer)
	{
		for (const TPair<FString, FFlockPlayerData>& Entry : Player.Value)
		{
			if (Entry.Value.Id == PlayerDataId)
			{
				OwningPlayerId = Player.Key;
				break;
			}
		}
		if (!OwningPlayerId.IsEmpty())
		{
			break;
		}
	}
	if (!OwningPlayerId.IsEmpty())
	{
		PlayerDataByPlayer.Remove(OwningPlayerId);
		DropReadsOnTheirWay(OwningPlayerId);
	}
}

void FFlockPlayerProvider::ClearCache()
{
	TemplatesById.Reset();
	TemplateIdByName.Reset();
	bAllTemplatesFetched = false;
	AllTemplateIds.Reset();
	ClearPlayerDataCache();
	DeleteSnapshotCategory(SnapshotCategory);
}

void FFlockPlayerProvider::ClearPlayerDataCache()
{
	PlayerDataByPlayer.Reset();
	DropReadsOnTheirWay(FString());
}

void FFlockPlayerProvider::DropReadsOnTheirWay(const FString& PlayerId)
{
	// One count for every player: a read keeps its answer only when nothing was dropped or applied while it was out.
	++PlayerDataDrops;
	if (PlayerId.IsEmpty())
	{
		PlayerDataReads.Reset();
	}
	else
	{
		PlayerDataReads.Remove(PlayerId);
	}
}

// ─────────────────────────────────── Internals ─────────────────────────────────

void FFlockPlayerProvider::IndexTemplate(const FFlockPlayerTemplateSchema& Template)
{
	if (Template.Id.IsEmpty())
	{
		return;
	}
	TemplatesById.Add(Template.Id, Template);
	if (!Template.Name.IsEmpty())
	{
		TemplateIdByName.Add(Template.Name, Template.Id);
	}
}

TArray<FFlockPlayerTemplateSchema> FFlockPlayerProvider::ResolveTemplates(const TArray<FString>& Ids) const
{
	TArray<FFlockPlayerTemplateSchema> Result;
	Result.Reserve(Ids.Num());
	for (const FString& Id : Ids)
	{
		if (const FFlockPlayerTemplateSchema* Template = TemplatesById.Find(Id))
		{
			Result.Add(*Template);
		}
	}
	return Result;
}

void FFlockPlayerProvider::GetOrFetchPlayerData(const FString& PlayerId, TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)> OnComplete)
{
	if (const TMap<FString, FFlockPlayerData>* Cached = PlayerDataByPlayer.Find(PlayerId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TMap<FString, FFlockPlayerData>>::Ok(*Cached));
		}
		return;
	}
	// Coalesce concurrent asks for the same player: only the first caller kicks off the paged fetch. A read sent under a sign-in
	// that has ended (the same player signing in again) is not joined: its later pages are refused, so this one goes out afresh.
	if (const TSharedRef<FPlayerDataRead>* OnItsWay = PlayerDataReads.Find(PlayerId))
	{
		if ((*OnItsWay)->SignInNumber == Session->GetSignInNumber())
		{
			(*OnItsWay)->Waiters.Add(MoveTemp(OnComplete));
			return;
		}
	}
	const TSharedRef<FPlayerDataRead> Read = MakeShared<FPlayerDataRead>();
	Read->Waiters.Add(MoveTemp(OnComplete));
	Read->DropsWhenSent = PlayerDataDrops;
	Read->SignInNumber = Session->GetSignInNumber();
	// Kept before it is sent, so a read that ends at once (its sign-in already over) is never left as the one on its way.
	PlayerDataReads.Add(PlayerId, Read);
	FetchPlayerDataPage(PlayerId, 1, MakeShared<TMap<FString, FFlockPlayerData>>(), Read);
}

void FFlockPlayerProvider::FetchPlayerDataPage(const FString& PlayerId, int32 Page, const TSharedRef<TMap<FString, FFlockPlayerData>>& Accumulated,
	const TSharedRef<FPlayerDataRead>& Read)
{
	static constexpr int32 PageSize = 100;
	const FString PlayerIdCopy = PlayerId;
	const TSharedRef<TMap<FString, FFlockPlayerData>> Acc = Accumulated;
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	ReadDataPage(PlayerIdCopy, Read->SignInNumber, Page, PageSize,
		[WeakSelf, PlayerIdCopy, Page, Acc, Read](TFlockResult<FFlockPlayerDataPage> Result)
		{
			const TSharedPtr<FFlockPlayerProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				// Torn down while the read was out: its callers still hear back.
				const TArray<TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)>> Waiters = MoveTemp(Read->Waiters);
				for (const TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)>& Waiter : Waiters)
				{
					if (Waiter)
					{
						Waiter(TFlockResult<TMap<FString, FFlockPlayerData>>::Fail(FFlockError::Make(EFlockErrorType::Cancelled,
							TEXT("Player provider was destroyed"))));
					}
				}
				return;
			}
			if (!Result.bSuccess)
			{
				Self->FinishPlayerDataRead(PlayerIdCopy, Read, TFlockResult<TMap<FString, FFlockPlayerData>>::Fail(Result.Error));
				return;
			}

			for (const FFlockPlayerData& Row : Result.Value.Items)
			{
				if (!Row.PlayerTemplateId.IsEmpty())
				{
					Acc->Add(Row.PlayerTemplateId, Row);
				}
			}

			const int32 Count = Result.Value.Items.Num();
			if (Count == 0 || Count < PageSize)
			{
				Self->FinishPlayerDataRead(PlayerIdCopy, Read, TFlockResult<TMap<FString, FFlockPlayerData>>::Ok(*Acc));
				return;
			}
			Self->FetchPlayerDataPage(PlayerIdCopy, Page + 1, Acc, Read);
		});
}

void FFlockPlayerProvider::FinishPlayerDataRead(const FString& PlayerId, const TSharedRef<FPlayerDataRead>& Read,
	const TFlockResult<TMap<FString, FFlockPlayerData>>& Result)
{
	// Only this read's own place is given up: a newer read sent after a drop keeps its place for the next caller to join.
	if (const TSharedRef<FPlayerDataRead>* OnItsWay = PlayerDataReads.Find(PlayerId))
	{
		if (*OnItsWay == Read)
		{
			PlayerDataReads.Remove(PlayerId);
		}
	}
	// An answer sent before a drop or a server row may be older than either, so it goes to its callers but is not kept.
	if (Result.bSuccess && Read->DropsWhenSent == PlayerDataDrops)
	{
		PlayerDataByPlayer.Add(PlayerId, Result.Value);
	}
	TArray<TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)>> Waiters = MoveTemp(Read->Waiters);
	Read->Waiters.Reset();
	for (TFunction<void(TFlockResult<TMap<FString, FFlockPlayerData>>)>& Waiter : Waiters)
	{
		if (Waiter)
		{
			Waiter(Result);
		}
	}
}
