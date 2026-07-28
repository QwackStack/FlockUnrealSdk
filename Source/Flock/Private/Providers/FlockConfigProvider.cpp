// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockConfigProvider.h"

#include "Http/FlockEndpoints.h"

const TCHAR* const FFlockConfigProvider::SnapshotCategory = TEXT("config");

FFlockConfigProvider::FFlockConfigProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const FString& InVersionedApiUrl, const TSharedPtr<FFlockSnapshotStore>& InSnapshotStore,
	const FString& InGameVersionId)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetSnapshotStore(InSnapshotStore, InGameVersionId);
}

// ─────────────────────────────────── Patches ───────────────────────────────────

void FFlockConfigProvider::GetAllPatches(TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnComplete)
{
	if (bAllPatchesFetched)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockGamePatchSchema>>::Ok(ResolvePatches(AllPatchIds)));
		}
		return;
	}

	const FString Key = TEXT("game_patch_all");
	if (!PatchListInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GamePatch);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGamePatchSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGamePatchSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch all patches"),
		[WeakSelf, Key](TFlockResult<TArray<FFlockGamePatchSchema>> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				Self->AllPatchIds.Reset();
				for (const FFlockGamePatchSchema& Patch : Result.Value)
				{
					Self->IndexPatch(Patch);
					if (!Patch.Id.IsEmpty())
					{
						Self->AllPatchIds.Add(Patch.Id);
					}
				}
				Self->bAllPatchesFetched = true;
			}
			Self->PatchListInFlight.Complete(Key, Result);
		});
}

void FFlockConfigProvider::GetPatchById(const FString& PatchId, TFunction<void(TFlockResult<FFlockGamePatchSchema>)> OnComplete)
{
	if (!RequireNotEmpty(PatchId, TEXT("Patch ID"), OnComplete))
	{
		return;
	}
	if (const FFlockGamePatchSchema* Cached = PatchesById.Find(PatchId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockGamePatchSchema>::Ok(*Cached));
		}
		return;
	}

	const FString Key = FString::Printf(TEXT("game_patch_%s"), *PatchId);
	if (!PatchInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GamePatchById(PatchId));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGamePatchSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockGamePatchSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGamePatchSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch patch"),
		[WeakSelf, Key](TFlockResult<FFlockGamePatchSchema> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				Self->IndexPatch(Result.Value);
			}
			Self->PatchInFlight.Complete(Key, Result);
		});
}

void FFlockConfigProvider::GetPatchesByConfig(const FString& ConfigId, TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnComplete)
{
	if (!RequireNotEmpty(ConfigId, TEXT("Config ID"), OnComplete))
	{
		return;
	}
	if (const TArray<FString>* Ids = PatchIdsByConfig.Find(ConfigId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockGamePatchSchema>>::Ok(ResolvePatches(*Ids)));
		}
		return;
	}

	const FString Key = FString::Printf(TEXT("game_patch_config_%s"), *ConfigId);
	if (!PatchListInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GamePatchByConfig(ConfigId));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGamePatchSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGamePatchSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch patches for config"),
		[WeakSelf, Key, ConfigId](TFlockResult<TArray<FFlockGamePatchSchema>> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				TArray<FString> Ids;
				for (const FFlockGamePatchSchema& Patch : Result.Value)
				{
					Self->IndexPatch(Patch);
					if (!Patch.Id.IsEmpty())
					{
						Ids.Add(Patch.Id);
					}
				}
				Self->PatchIdsByConfig.Add(ConfigId, Ids);
			}
			Self->PatchListInFlight.Complete(Key, Result);
		});
}

// ─────────────────────────────────── Configs ───────────────────────────────────

void FFlockConfigProvider::GetConfigById(const FString& ConfigId, TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnComplete)
{
	if (!RequireNotEmpty(ConfigId, TEXT("Config ID"), OnComplete))
	{
		return;
	}
	if (const FFlockGameConfigSchema* Cached = ConfigsById.Find(ConfigId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockGameConfigSchema>::Ok(*Cached));
		}
		return;
	}

	const FString Key = FString::Printf(TEXT("game_config_%s"), *ConfigId);
	if (!ConfigInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GameConfigById(ConfigId));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameConfigSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch config"),
		[WeakSelf, Key, ConfigId](TFlockResult<FFlockGameConfigSchema> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			// Keyed by the requested id (not the returned one), so the next call with this id is a hit.
			if (Result.bSuccess)
			{
				Self->ConfigsById.Add(ConfigId, Result.Value);
			}
			Self->ConfigInFlight.Complete(Key, Result);
		});
}

void FFlockConfigProvider::GetConfigByName(const FString& Name, TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnComplete)
{
	if (!RequireNotEmpty(Name, TEXT("Config Name"), OnComplete))
	{
		return;
	}
	if (const FString* Id = ConfigIdByName.Find(Name))
	{
		if (const FFlockGameConfigSchema* Cached = ConfigsById.Find(*Id))
		{
			if (OnComplete)
			{
				OnComplete(TFlockResult<FFlockGameConfigSchema>::Ok(*Cached));
			}
			return;
		}
	}

	const FString Key = FString::Printf(TEXT("game_config_name_%s"), *Name);
	if (!ConfigInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GameConfigByName(Name));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameConfigSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch config by name"),
		[WeakSelf, Key, Name](TFlockResult<FFlockGameConfigSchema> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				Self->IndexConfig(Result.Value);
				if (!Result.Value.Id.IsEmpty())
				{
					Self->ConfigIdByName.Add(Name, Result.Value.Id);
				}
			}
			Self->ConfigInFlight.Complete(Key, Result);
		});
}

void FFlockConfigProvider::GetConfigsByTag(EFlockConfigTag Tag, TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnComplete)
{
	const FString Wire = FlockConfigTagToWire(Tag);
	const FString TagKey = Wire.IsEmpty() ? TEXT("any") : Wire;
	if (const TArray<FString>* Ids = ConfigIdsByTag.Find(TagKey))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockGameConfigSchema>>::Ok(ResolveConfigs(*Ids)));
		}
		return;
	}

	const FString Key = FString::Printf(TEXT("game_config_tag_%s"), *TagKey);
	if (!ConfigListInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	FString Path = FlockEndpoints::GameConfig;
	if (!Wire.IsEmpty())
	{
		Path += FString::Printf(TEXT("?tag=%s"), *Wire);
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(Path);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGameConfigSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch configs by tag"),
		[WeakSelf, Key, TagKey](TFlockResult<TArray<FFlockGameConfigSchema>> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				TArray<FString> Ids;
				for (const FFlockGameConfigSchema& Config : Result.Value)
				{
					Self->IndexConfig(Config);
					if (!Config.Id.IsEmpty())
					{
						Ids.Add(Config.Id);
					}
				}
				Self->ConfigIdsByTag.Add(TagKey, Ids);
			}
			Self->ConfigListInFlight.Complete(Key, Result);
		});
}

void FFlockConfigProvider::GetConfigsByVersionTag(EFlockConfigTag Tag, TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnComplete)
{
	const FString Wire = FlockConfigTagToWire(Tag);
	const FString TagKey = Wire.IsEmpty() ? TEXT("any") : Wire;
	if (const TArray<FString>* Ids = ConfigIdsByVersionTag.Find(TagKey))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TArray<FFlockGameConfigSchema>>::Ok(ResolveConfigs(*Ids)));
		}
		return;
	}

	const FString Key = FString::Printf(TEXT("game_config_version_tag_%s"), *TagKey);
	if (!ConfigListInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	FString Path = FlockEndpoints::GameConfigVersion;
	if (!Wire.IsEmpty())
	{
		Path += FString::Printf(TEXT("?tag=%s"), *Wire);
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(Path);
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGameConfigSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		TEXT("Fetch configs by version tag"),
		[WeakSelf, Key, TagKey](TFlockResult<TArray<FFlockGameConfigSchema>> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				TArray<FString> Ids;
				for (const FFlockGameConfigSchema& Config : Result.Value)
				{
					Self->IndexConfig(Config);
					if (!Config.Id.IsEmpty())
					{
						Ids.Add(Config.Id);
					}
				}
				Self->ConfigIdsByVersionTag.Add(TagKey, Ids);
			}
			Self->ConfigListInFlight.Complete(Key, Result);
		});
}

void FFlockConfigProvider::GetPlayerFeatures(const FString& PlayerId, TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnComplete)
{
	// Empty = the signed-in player, matching every other Player ID argument in the SDK.
	const FString ResolvedPlayerId = PlayerId.IsEmpty() ? Session->GetPlayerId() : PlayerId;
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID (sign in first)"), OnComplete))
	{
		return;
	}
	if (const FFlockGameConfigSchema* Cached = PlayerFeaturesByPlayer.Find(ResolvedPlayerId))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockGameConfigSchema>::Ok(*Cached));
		}
		return;
	}

	const FString Key = FString::Printf(TEXT("game_config_features_%s"), *ResolvedPlayerId);
	if (!ConfigInFlight.Register(Key, MoveTemp(OnComplete)))
	{
		return;
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const FString Url = MakeUrl(FlockEndpoints::GameConfigPlayerFeatures(ResolvedPlayerId));
	const TMap<FString, FString> Headers = HeadersNow();
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	// No snapshot: per-player resolved features must not be served stale, so this is a plain Execute.
	Execute<FFlockGameConfigSchema>(
		[ClientRef, Url, Headers](TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameConfigSchema>(Url, Headers, MoveTemp(OnAttempt));
		},
		[WeakSelf, Key, ResolvedPlayerId](TFlockResult<FFlockGameConfigSchema> Result)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (Result.bSuccess)
			{
				Self->PlayerFeaturesByPlayer.Add(ResolvedPlayerId, Result.Value);
			}
			Self->ConfigInFlight.Complete(Key, Result);
		},
		TEXT("Fetch player features"));
}

void FFlockConfigProvider::ResolveConfigData(const FString& ConfigId, TFunction<void(TFlockResult<FFlockStructuredData>)> OnComplete)
{
	if (!RequireNotEmpty(ConfigId, TEXT("Config ID"), OnComplete))
	{
		return;
	}

	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();
	GetPatchesByConfig(ConfigId,
		[WeakSelf, ConfigId, OnComplete](TFlockResult<TArray<FFlockGamePatchSchema>> PatchResult)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			// A patch for this game version wins.
			if (PatchResult.bSuccess && PatchResult.Value.Num() > 0)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockStructuredData>::Ok(PatchResult.Value[0].Data));
				}
				return;
			}
			// A failed patch fetch (e.g. offline with nothing cached) propagates — don't silently fall back.
			if (!PatchResult.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockStructuredData>::Fail(PatchResult.Error));
				}
				return;
			}
			// No patch on this version: fall back to the config's own base data.
			Self->GetConfigById(ConfigId,
				[OnComplete](TFlockResult<FFlockGameConfigSchema> ConfigResult)
				{
					if (!OnComplete)
					{
						return;
					}
					if (ConfigResult.bSuccess)
					{
						OnComplete(TFlockResult<FFlockStructuredData>::Ok(ConfigResult.Value.Data));
					}
					else
					{
						OnComplete(TFlockResult<FFlockStructuredData>::Fail(ConfigResult.Error));
					}
				});
		});
}

void FFlockConfigProvider::ClearCache()
{
	PatchesById.Reset();
	ConfigsById.Reset();
	PatchIdsByConfig.Reset();
	ConfigIdsByTag.Reset();
	ConfigIdsByVersionTag.Reset();
	ConfigIdByName.Reset();
	PlayerFeaturesByPlayer.Reset();
	bAllPatchesFetched = false;
	AllPatchIds.Reset();
	DeleteSnapshotCategory(SnapshotCategory);
}

// ─────────────────────────────────── Indexing ──────────────────────────────────

void FFlockConfigProvider::IndexPatch(const FFlockGamePatchSchema& Patch)
{
	if (!Patch.Id.IsEmpty())
	{
		PatchesById.Add(Patch.Id, Patch);
	}
}

void FFlockConfigProvider::IndexConfig(const FFlockGameConfigSchema& Config)
{
	if (!Config.Id.IsEmpty())
	{
		ConfigsById.Add(Config.Id, Config);
	}
}

TArray<FFlockGamePatchSchema> FFlockConfigProvider::ResolvePatches(const TArray<FString>& Ids) const
{
	TArray<FFlockGamePatchSchema> Result;
	Result.Reserve(Ids.Num());
	for (const FString& Id : Ids)
	{
		if (const FFlockGamePatchSchema* Patch = PatchesById.Find(Id))
		{
			Result.Add(*Patch);
		}
	}
	return Result;
}

TArray<FFlockGameConfigSchema> FFlockConfigProvider::ResolveConfigs(const TArray<FString>& Ids) const
{
	TArray<FFlockGameConfigSchema> Result;
	Result.Reserve(Ids.Num());
	for (const FString& Id : Ids)
	{
		if (const FFlockGameConfigSchema* Config = ConfigsById.Find(Id))
		{
			Result.Add(*Config);
		}
	}
	return Result;
}
