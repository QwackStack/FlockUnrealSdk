// Copyright 2022, Qwacks. All Rights Reserved.

#include "Providers/FlockPlayerProvider.h"

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
	const FString Url = MakeUrl(FlockEndpoints::PlayerDataById(PlayerDataId));
	const TMap<FString, FString> Headers = HeadersNow();

	// Enveloped ({error,response,result}); never cached — a direct row read is always fresh.
	Execute<FFlockPlayerData>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockPlayerData>)> OnAttempt)
		{
			return ClientRef->Get<FFlockPlayerData>(Url, Headers, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("Fetch player data"));
}

void FFlockPlayerProvider::GetAllData(const FString& PlayerId, int32 Page, int32 Limit,
	TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnComplete)
{
	FString Path = FString::Printf(TEXT("%s?page=%d&limit=%d"), FlockEndpoints::PlayerData, Page, Limit);
	if (!PlayerId.IsEmpty())
	{
		Path += FString::Printf(TEXT("&player_id=%s"), *PlayerId);
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(Path);
	const TMap<FString, FString> Headers = HeadersNow();

	// Bare paginated ({items,total,page,limit}); never cached. GetPaged returns the template page — copy it
	// into the concrete, Blueprint-able page struct.
	Execute<FFlockPlayerDataPage>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockPlayerDataPage>)> OnAttempt)
		{
			return ClientRef->GetPaged<FFlockPlayerData>(Url, Headers,
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
		},
		MoveTemp(OnComplete), TEXT("Fetch player data list"));
}

void FFlockPlayerProvider::GetMyDataByTemplate(const FString& PlayerTemplateId, TFunction<void(TFlockResult<FFlockPlayerData>)> OnComplete)
{
	if (!RequireNotEmpty(PlayerTemplateId, TEXT("Player Template ID"), OnComplete))
	{
		return;
	}
	const FString PlayerId = Session->GetPlayerId();
	if (!RequireNotEmpty(PlayerId, TEXT("Current Player ID (sign in first)"), OnComplete))
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
			// No row for this template — an empty record (empty Id), mirroring Unity's null return.
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
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	// Enveloped list ({error,response,result:[...]}) — GetList unwraps `result` as a bare array.
	FetchListWithSnapshot<FFlockPlayerTemplateSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockPlayerTemplateSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockPlayerTemplateSchema>(Url, Headers, MoveTemp(OnAttempt));
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
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	// Memoize only — matching Unity, by-id template reads are not snapshotted.
	Execute<FFlockPlayerTemplateSchema>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockPlayerTemplateSchema>(Url, Headers, MoveTemp(OnAttempt));
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
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	Execute<FFlockPlayerTemplateSchema>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockPlayerTemplateSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockPlayerTemplateSchema>(Url, Headers, MoveTemp(OnAttempt));
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
			OnComplete(TFlockResult<FFlockPlayerTemplateSchema>::Fail(FFlockError::Make(EFlockErrorType::Validation,
				FString::Printf(TEXT("No player template tagged '%s' for this game."), *TagCopy))));
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
	const TMap<FString, FString> Headers = HeadersNow();

	// Enveloped list; never cached (a template's rows change as players play).
	Execute<TArray<FFlockPlayerData>>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockPlayerData>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockPlayerData>(Url, Headers, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("Fetch player data for template"));
}

// ─────────────────────────────────────── Bans ──────────────────────────────────

void FFlockPlayerProvider::GetBan(const FString& PlayerId, TFunction<void(TFlockResult<FFlockPlayerBan>)> OnComplete)
{
	const FString ResolvedPlayerId = PlayerId.IsEmpty() ? Session->GetPlayerId() : PlayerId;
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID (sign in first)"), OnComplete))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FString::Printf(TEXT("%s?player_id=%s"), FlockEndpoints::PlayerBan, *ResolvedPlayerId));
	const TMap<FString, FString> Headers = HeadersNow();

	// Enveloped but nullable (Union[PlayerBanSchema, None]) — GetRaw into the wrapper, whose custom parse
	// treats a null `result` as "not banned". Never cached: a ban can change server-side at any time.
	Execute<FFlockPlayerBanResponse>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockPlayerBanResponse>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockPlayerBanResponse>(Url, Headers, MoveTemp(OnAttempt));
		},
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

void FFlockPlayerProvider::ClearCache()
{
	TemplatesById.Reset();
	TemplateIdByName.Reset();
	bAllTemplatesFetched = false;
	AllTemplateIds.Reset();
	PlayerDataByPlayer.Reset();
	DeleteSnapshotCategory(SnapshotCategory);
}

void FFlockPlayerProvider::ClearPlayerDataCache()
{
	PlayerDataByPlayer.Reset();
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
	// Coalesce concurrent asks for the same player: only the first caller kicks off the paged fetch.
	if (!PlayerDataInFlight.Register(PlayerId, MoveTemp(OnComplete)))
	{
		return;
	}
	FetchPlayerDataPage(PlayerId, 1, MakeShared<TMap<FString, FFlockPlayerData>>());
}

void FFlockPlayerProvider::FetchPlayerDataPage(const FString& PlayerId, int32 Page, const TSharedRef<TMap<FString, FFlockPlayerData>>& Accumulated)
{
	static constexpr int32 PageSize = 100;
	const FString PlayerIdCopy = PlayerId;
	const TSharedRef<TMap<FString, FFlockPlayerData>> Acc = Accumulated;
	TWeakPtr<FFlockPlayerProvider> WeakSelf = AsShared();

	GetAllData(PlayerIdCopy, Page, PageSize,
		[WeakSelf, PlayerIdCopy, Page, Acc](TFlockResult<FFlockPlayerDataPage> Result)
		{
			const TSharedPtr<FFlockPlayerProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!Result.bSuccess)
			{
				Self->PlayerDataInFlight.Complete(PlayerIdCopy,
					TFlockResult<TMap<FString, FFlockPlayerData>>::Fail(Result.Error));
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
				// Last page: cache the whole map and fan the result out to every coalesced caller.
				Self->PlayerDataByPlayer.Add(PlayerIdCopy, *Acc);
				Self->PlayerDataInFlight.Complete(PlayerIdCopy,
					TFlockResult<TMap<FString, FFlockPlayerData>>::Ok(*Acc));
				return;
			}
			Self->FetchPlayerDataPage(PlayerIdCopy, Page + 1, Acc);
		});
}
