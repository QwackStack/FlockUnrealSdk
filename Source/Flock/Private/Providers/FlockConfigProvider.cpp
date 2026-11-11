// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockConfigProvider.h"
#include "Http/FlockErrorHints.h"

#include "Http/FlockEndpoints.h"
#include "Http/FlockJsonUtils.h"

const TCHAR* const FFlockConfigProvider::SnapshotCategory = TEXT("config");

namespace
{
	/**
	 * A 2xx JSON answer whose `result` is null or missing is no answer, not a refusal: it is tried again, then the player's saved
	 * copy or the game's read answers, and the player's read is asked again after the pause. A body that is not JSON, or a result
	 * that cannot be read, stays a failure of its own.
	 */
	TFlockResult<FFlockGameConfigSchema> AsNoAnswerWhenItHasNoResult(TFlockResult<FFlockGameConfigSchema> Result)
	{
		if (Result.bSuccess || Result.Error.Type != EFlockErrorType::Serialization)
		{
			return Result;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FFlockJsonUtils::TryParseObject(Result.Error.Body, Root) || !Root.IsValid())
		{
			return Result;
		}
		const TSharedPtr<FJsonValue> Found = Root->TryGetField(TEXT("result"));
		if (Found.IsValid() && !Found->IsNull())
		{
			return Result;
		}
		return TFlockResult<FFlockGameConfigSchema>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Network,
			TEXT("The server's answer has no result in it"), FFlockErrorHints::NotFlocksAnswer()));
	}
}

FFlockConfigProvider::FFlockConfigProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGamePatchSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGamePatchSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGamePatchSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockGamePatchSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGamePatchSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGamePatchSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<TArray<FFlockGamePatchSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGamePatchSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameConfigSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameConfigSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGameConfigSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	FetchListWithSnapshot<FFlockGameConfigSchema>(SnapshotCategory, Key,
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<TArray<FFlockGameConfigSchema>>)> OnAttempt)
		{
			return ClientRef->GetList<FFlockGameConfigSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	if (!RequireNotEmpty(ResolvedPlayerId, TEXT("Player ID"), OnComplete, FFlockErrorHints::SignInFirst()))
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
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();

	// No snapshot on disk: a player's features are kept in memory for the run only (PlayerFeaturesByPlayer).
	Execute<FFlockGameConfigSchema>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnAttempt)
		{
			return ClientRef->Get<FFlockGameConfigSchema>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
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
	ReadForTheSignedInPlayer(ConfigId,
		[WeakSelf, ConfigId, OnComplete](TFlockResult<TOptional<FFlockGameConfigSchema>> ForThePlayer)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			if (!ForThePlayer.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockStructuredData>::Fail(ForThePlayer.Error));
				}
				return;
			}
			if (ForThePlayer.Value.IsSet())
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockStructuredData>::Ok(ForThePlayer.Value.GetValue().Data));
				}
				return;
			}
			Self->ResolveForTheGame(ConfigId, OnComplete);
		});
}

void FFlockConfigProvider::ResolveForTheGame(const FString& ConfigId, TFunction<void(TFlockResult<FFlockStructuredData>)> OnComplete)
{
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

// ─────────────────────────────── The signed-in player ──────────────────────────────

void FFlockConfigProvider::ReadForTheSignedInPlayer(const FString& ConfigId,
	TFunction<void(TFlockResult<TOptional<FFlockGameConfigSchema>>)> Then)
{
	using FPlayersRead = TFlockResult<TOptional<FFlockGameConfigSchema>>;
	const FString PlayerId = Session->GetPlayerId();
	if (!Session->IsAuthenticated() || PlayerId.IsEmpty())
	{
		Then(FPlayersRead::Ok(TOptional<FFlockGameConfigSchema>()));
		return;
	}
	const int32 SignIn = Session->GetSignInNumber();
	if (const TOptional<FFlockGameConfigSchema>* Held = PlayerConfigsOf(SignIn).Find(ConfigId))
	{
		Then(FPlayersRead::Ok(*Held));
		return;
	}

	const FString Key = FString::Printf(TEXT("player_config_%s_%s"), *ConfigId, *PlayerId);
	const TOptional<FFlockGameConfigSchema> Saved = SavedPlayerConfig(ConfigId, Key);
	// No connection, or no answer a moment ago: the player's saved copy, else the game's read, at once.
	const FDateTime* PausedUntil = PlayerReadsPausedUntil.Find(ConfigId);
	if (!IsServerReachable() || (PausedUntil != nullptr && FDateTime::UtcNow() < *PausedUntil))
	{
		Then(FPlayersRead::Ok(Saved));
		return;
	}

	const int32 ClearsWhenAsked = Clears;
	const TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();
	FetchForThePlayer(ConfigId, SignIn, Saved.IsSet(),
		[WeakSelf, ConfigId, SignIn, Key, Saved, ClearsWhenAsked, Then](FPlayersRead Answer)
		{
			const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			// The sign-in ended before a retry went out, or while the read was on its way: the answer is kept nowhere, and the
			// read runs again for whoever is signed in now.
			if (Self->Session->GetSignInNumber() != SignIn)
			{
				Self->ReadForTheSignedInPlayer(ConfigId, Then);
				return;
			}
			const bool bKeep = ClearsWhenAsked == Self->Clears;
			if (!Answer.bSuccess)
			{
				const FFlockError& Failed = Answer.Error;
				if (Failed.Type == EFlockErrorType::Cancelled)
				{
					Then(Answer);
					return;
				}
				const bool bNoAnswer = (Failed.Type == EFlockErrorType::Network || Failed.Type == EFlockErrorType::Timeout
					|| Failed.Type == EFlockErrorType::Connection) && !FFlockError::IsPermanentStatus(Failed.StatusCode);
				if (bNoAnswer)
				{
					if (bKeep)
					{
						Self->PlayerConfigsOf(SignIn);
						Self->PlayerReadsPausedUntil.Add(ConfigId,
							FDateTime::UtcNow() + FTimespan::FromSeconds(Self->PlayerReadPauseAfterNoAnswerSeconds));
					}
					Self->Logger->LogDebug(FString::Printf(TEXT("Config %s: the player's read got no answer, so %s answers for now (%s)"),
						*ConfigId, Saved.IsSet() ? TEXT("the player's saved copy") : TEXT("the game's read"),
						Failed.StatusCode != 0 ? *FString::FromInt(Failed.StatusCode) : TEXT("no answer")));
					Then(FPlayersRead::Ok(Saved));
					return;
				}
				// A refusal, or a failure that will not change during this sign-in: the game's read answers for the rest of it.
				if (bKeep)
				{
					const bool bNotForThePlayersRead = Failed.ErrorCode == EFlockErrorCode::GameConfigPlayerNotFound
						|| Failed.ErrorCode == EFlockErrorCode::GameConfigPlayerNoGameVersion
						|| Failed.ErrorCode == EFlockErrorCode::GameConfigConfigNotFound;
					Self->LeaveToTheGamesRead(SignIn, ConfigId, Key, bNotForThePlayersRead);
				}
				if (Failed.ErrorCode == EFlockErrorCode::GameConfigPlayerNoGameVersion && Self->WarnedOfNoGameVersionSignIn != SignIn)
				{
					Self->WarnedOfNoGameVersionSignIn = SignIn;
					Self->Logger->LogWarning(TEXT("Config values are the game's, not the signed-in player's: the server got no Game Version ")
						TEXT("with the request, so it cannot read the config as this player sees it (the overrides of their segments and ")
						TEXT("their own). Fix: resolve the Game Version (Tools > Flock > Resolve Game Version, or Project Settings > Plugins > ")
						TEXT("Flock SDK Settings) so the build sends it on every request; a proxy that drops the X-Game-Version-ID header does ")
						TEXT("the same."));
				}
				else
				{
					Self->Logger->LogDebug(FString::Printf(TEXT("Config %s: the player's read cannot answer it, so the game's read does for this sign-in (%s)"),
						*ConfigId, Failed.Code.IsEmpty() ? *FString::FromInt(Failed.StatusCode) : *Failed.Code));
				}
				Then(FPlayersRead::Ok(TOptional<FFlockGameConfigSchema>()));
				return;
			}

			// Another config's values (a rename during the launch, another version's config of that name) are not this one's.
			const bool bForThisConfig = Answer.Value.IsSet() && Answer.Value.GetValue().Id.Equals(ConfigId, ESearchCase::CaseSensitive);
			if (bKeep)
			{
				if (bForThisConfig)
				{
					Self->PlayerConfigsOf(SignIn).Add(ConfigId, Answer.Value);
					Self->SavedPlayerConfigsById.Add(ConfigId, Answer.Value);
					FString Payload;
					if (Self->GetSnapshotStore().IsValid() && FFlockJsonUtils::StructToPlainJson(Answer.Value.GetValue(), Payload))
					{
						Self->GetSnapshotStore()->Write(Self->GetSnapshotScope(SnapshotCategory), Key, Payload);
					}
				}
				else
				{
					Self->LeaveToTheGamesRead(SignIn, ConfigId, Key, Answer.Value.IsSet());
				}
			}
			Then(FPlayersRead::Ok(bForThisConfig ? Answer.Value : TOptional<FFlockGameConfigSchema>()));
		});
}

void FFlockConfigProvider::FetchForThePlayer(const FString& ConfigId, int32 SignInNumber, bool bHasSavedCopy,
	TFunction<void(TFlockResult<TOptional<FFlockGameConfigSchema>>)> Then)
{
	using FPlayersRead = TFlockResult<TOptional<FFlockGameConfigSchema>>;
	const TWeakPtr<FFlockConfigProvider> WeakSelf = AsShared();
	GetConfigById(ConfigId, [WeakSelf, SignInNumber, bHasSavedCopy, Then](TFlockResult<FFlockGameConfigSchema> Config)
	{
		const TSharedPtr<FFlockConfigProvider> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		if (!Config.bSuccess)
		{
			Then(FPlayersRead::Fail(Config.Error));
			return;
		}
		// The server resolves only gameplay and feature configs for a player; any other comes back as stored, without its patch.
		const FString& Tag = Config.Value.Tag;
		const FString Name = Config.Value.Name;
		if (Name.IsEmpty() || !(Tag.Equals(TEXT("gameplay"), ESearchCase::IgnoreCase) || Tag.Equals(TEXT("feature"), ESearchCase::IgnoreCase)))
		{
			Then(FPlayersRead::Ok(TOptional<FFlockGameConfigSchema>()));
			return;
		}

		const TSharedRef<FFlockHttpClient> ClientRef = Self->Client;
		const TSharedRef<FFlockAuthSession> SessionRef = Self->Session;
		const FString Url = Self->MakeUrl(FlockEndpoints::GameConfigForMeByName(Name));
		FFlockRetryHandler::FOperation<FFlockGameConfigSchema> Send =
			[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockGameConfigSchema>)> OnAttempt)
			{
				return ClientRef->Get<FFlockGameConfigSchema>(Url, SessionRef->GetAuthHeaders(),
					[OnAttempt = MoveTemp(OnAttempt)](TFlockResult<FFlockGameConfigSchema> Result)
					{
						OnAttempt(AsNoAnswerWhenItHasNoResult(MoveTemp(Result)));
					});
			};
		// A failure here is answered by the game's read, so it is no error to log; the caller says what happened.
		Self->Execute<FFlockGameConfigSchema>(ForSignInOnly<FFlockGameConfigSchema>(SessionRef, SignInNumber, MoveTemp(Send)),
			[Then](TFlockResult<FFlockGameConfigSchema> Result)
			{
				Then(Result.bSuccess ? FPlayersRead::Ok(TOptional<FFlockGameConfigSchema>(Result.Value)) : FPlayersRead::Fail(Result.Error));
			},
			FString::Printf(TEXT("Fetch config %s for the signed-in player"), *Name), /*bIdempotent*/ true,
			/*MaxRetriesOverride*/ bHasSavedCopy ? 0 : -1, /*bAllowAuthRetry*/ true, [](const FFlockError&) { return true; });
	});
}

void FFlockConfigProvider::LeaveToTheGamesRead(int32 SignInNumber, const FString& ConfigId, const FString& Key, bool bDeleteTheSavedCopy)
{
	PlayerConfigsOf(SignInNumber).Add(ConfigId, TOptional<FFlockGameConfigSchema>());
	if (!bDeleteTheSavedCopy)
	{
		return;
	}
	SavedPlayerConfigsById.Add(ConfigId, TOptional<FFlockGameConfigSchema>());
	if (GetSnapshotStore().IsValid())
	{
		GetSnapshotStore()->DeleteKey(GetSnapshotScope(SnapshotCategory), Key);
	}
}

TOptional<FFlockGameConfigSchema> FFlockConfigProvider::SavedPlayerConfig(const FString& ConfigId, const FString& Key)
{
	if (const TOptional<FFlockGameConfigSchema>* Known = SavedPlayerConfigsById.Find(ConfigId))
	{
		return *Known;
	}
	TOptional<FFlockGameConfigSchema> Saved;
	FString Payload;
	FFlockGameConfigSchema Config;
	if (GetSnapshotStore().IsValid() && GetSnapshotStore()->TryRead(GetSnapshotScope(SnapshotCategory), Key, Payload)
		&& FFlockJsonUtils::PlainJsonToStruct(Payload, Config))
	{
		Saved = Config;
	}
	SavedPlayerConfigsById.Add(ConfigId, Saved);
	return Saved;
}

TMap<FString, TOptional<FFlockGameConfigSchema>>& FFlockConfigProvider::PlayerConfigsOf(int32 SignInNumber)
{
	if (PlayerConfigsSignIn != SignInNumber)
	{
		PlayerConfigsById.Reset();
		SavedPlayerConfigsById.Reset();
		PlayerReadsPausedUntil.Reset();
		PlayerConfigsSignIn = SignInNumber;
	}
	return PlayerConfigsById;
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
	PlayerConfigsById.Reset();
	SavedPlayerConfigsById.Reset();
	PlayerReadsPausedUntil.Reset();
	++Clears;
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
