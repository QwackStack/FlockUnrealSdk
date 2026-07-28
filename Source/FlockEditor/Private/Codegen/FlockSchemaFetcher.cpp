// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Codegen/FlockSchemaFetcher.h"

#include "Config/FlockConfig.h"
#include "FlockLogger.h"
#include "FlockSubsystem.h"
#include "Http/FlockEndpoints.h"
#include "Models/FlockGameModels.h"
#include "Version/FlockVersionResolver.h"

namespace
{
	/** One page of shops per request; the list is small, so this is about bounding a response, not tuning. */
	constexpr int32 ShopPageSize = 100;

	/** Names the step that failed without losing the transport's own classification or status code. */
	FFlockError Contextualize(const FString& Step, const FFlockError& Error)
	{
		return FFlockError::Make(Error.Type, FString::Printf(TEXT("%s: %s"), *Step, *Error.Message),
			Error.StatusCode, Error.Body, Error.Code, Error.ServerMessage);
	}

	/**
	 * The chain's shared state. Held by shared ref so each continuation keeps it alive without any of them
	 * owning it, and so an abort can be expressed as "report and stop" from wherever it happens.
	 */
	struct FFetchState : public TSharedFromThis<FFetchState>
	{
		TSharedRef<FFlockHttpClient> Client;
		FString VersionedUrl;
		TMap<FString, FString> Headers;
		FFlockSchemaSnapshot Snapshot;
		FFlockSchemaFetcher::FOnSchemaFetched OnComplete;

		/** Index of the next shop needing an item backfill. */
		int32 ShopCursor = 0;

		explicit FFetchState(const TSharedRef<FFlockHttpClient>& InClient)
			: Client(InClient)
		{
		}

		FString Url(const FString& Path) const { return FString::Printf(TEXT("%s/%s"), *VersionedUrl, *Path); }

		void Fail(const FString& Step, const FFlockError& Error)
		{
			OnComplete.ExecuteIfBound(TFlockResult<FFlockSchemaSnapshot>::Fail(Contextualize(Step, Error)));
		}

		void Succeed()
		{
			Snapshot.FetchedAtUtc = FDateTime::UtcNow();
			OnComplete.ExecuteIfBound(TFlockResult<FFlockSchemaSnapshot>::Ok(Snapshot));
		}
	};

	// Forward declarations — the chain is a sequence of free functions rather than nested lambdas, so each
	// step reads on its own and the abort path is one call.
	void FetchTemplates(const TSharedRef<FFetchState>& State);
	void FetchConfigs(const TSharedRef<FFetchState>& State);
	void FetchShopPage(const TSharedRef<FFetchState>& State, int32 Page);
	void BackfillShopItems(const TSharedRef<FFetchState>& State);

	void FetchTemplates(const TSharedRef<FFetchState>& State)
	{
		// Enveloped list ({error,response,result:[…]}).
		State->Client->GetList<FFlockPlayerTemplateSchema>(State->Url(FlockEndpoints::PlayerTemplate), State->Headers,
			[State](TFlockResult<TArray<FFlockPlayerTemplateSchema>> Result)
			{
				if (!Result.bSuccess)
				{
					State->Fail(TEXT("Fetch player templates"), Result.Error);
					return;
				}
				State->Snapshot.PlayerTemplates = MoveTemp(Result.Value);
				FetchConfigs(State);
			});
	}

	void FetchConfigs(const TSharedRef<FFetchState>& State)
	{
		// game_config/version is the whole set for this game version — the same route the config provider
		// uses for its by-tag sweep, and enveloped like it.
		State->Client->GetList<FFlockGameConfigSchema>(State->Url(FlockEndpoints::GameConfigVersion), State->Headers,
			[State](TFlockResult<TArray<FFlockGameConfigSchema>> Result)
			{
				if (!Result.bSuccess)
				{
					State->Fail(TEXT("Fetch game configs"), Result.Error);
					return;
				}
				State->Snapshot.GameConfigs = MoveTemp(Result.Value);
				FetchShopPage(State, 1);
			});
	}

	void FetchShopPage(const TSharedRef<FFetchState>& State, int32 Page)
	{
		const FString Path = FString::Printf(TEXT("%s?page=%d&limit=%d"), FlockEndpoints::Shop, Page, ShopPageSize);
		State->Client->GetPaged<FFlockShop>(State->Url(Path), State->Headers,
			[State, Page](TFlockResult<TFlockPage<FFlockShop>> Result)
			{
				if (!Result.bSuccess)
				{
					State->Fail(TEXT("Fetch shops"), Result.Error);
					return;
				}

				State->Snapshot.Shops.Append(Result.Value.Items);

				// A short page is the last one; an empty first page just means no shops.
				if (Result.Value.Items.Num() >= ShopPageSize)
				{
					FetchShopPage(State, Page + 1);
					return;
				}
				BackfillShopItems(State);
			});
	}

	void BackfillShopItems(const TSharedRef<FFetchState>& State)
	{
		// The shop list may or may not embed items depending on the route's shape, so top up only the
		// shops that came back without them. Walks the list one shop at a time; the cursor is on the state
		// so a completion can resume where it left off.
		while (State->ShopCursor < State->Snapshot.Shops.Num())
		{
			const FFlockShop& Shop = State->Snapshot.Shops[State->ShopCursor];
			if (Shop.Id.IsEmpty() || Shop.ShopItems.Num() > 0)
			{
				++State->ShopCursor;
				continue;
			}

			const int32 Index = State->ShopCursor;
			const FString ShopId = Shop.Id;
			State->Client->GetList<FFlockShopItem>(State->Url(FlockEndpoints::ShopItemsByShop(ShopId)), State->Headers,
				[State, Index, ShopId](TFlockResult<TArray<FFlockShopItem>> Result)
				{
					if (!Result.bSuccess)
					{
						State->Fail(FString::Printf(TEXT("Fetch items for shop %s"), *ShopId), Result.Error);
						return;
					}
					if (State->Snapshot.Shops.IsValidIndex(Index))
					{
						State->Snapshot.Shops[Index].ShopItems = MoveTemp(Result.Value);
					}
					State->ShopCursor = Index + 1;
					BackfillShopItems(State);
				});
			return; // resumed by the completion above
		}

		State->Succeed();
	}
}

void FFlockSchemaFetcher::Fetch(const TSharedRef<FFlockHttpClient>& Client, const FString& ApiUrl, const FString& ApiKey,
	const FString& GameVersionName, const FString& BakedGameVersionId, FOnSchemaFetched OnComplete)
{
	if (GameVersionName.IsEmpty())
	{
		OnComplete.ExecuteIfBound(TFlockResult<FFlockSchemaSnapshot>::Fail(FFlockError::Make(
			EFlockErrorType::Validation, TEXT("Game Version is empty in Project Settings > Flock SDK."))));
		return;
	}

	TSharedRef<FFetchState> State = MakeShared<FFetchState>(Client);
	State->OnComplete = OnComplete;
	State->Snapshot.BakedGameVersionId = BakedGameVersionId;
	State->VersionedUrl = FString::Printf(TEXT("%s/%s"),
		*ApiUrl.TrimEnd().TrimChar('/'), *UFlockSubsystem::ApiVersion);
	State->Headers.Add(TEXT("X-Flock-API-Key"), ApiKey);

	// Resolve by name first: the version id scopes every request after it, and resolving (rather than
	// trusting the baked id) is what catches a bake that has gone stale. Enveloped, like the runtime's.
	Client->Get<FFlockGameVersionSchema>(FFlockVersionResolver::ByNameUrl(ApiUrl, GameVersionName), State->Headers,
		[State, GameVersionName](TFlockResult<FFlockGameVersionSchema> Result)
		{
			if (!Result.bSuccess)
			{
				State->Fail(TEXT("Resolve game version"), Result.Error);
				return;
			}
			if (Result.Value.Id.IsEmpty())
			{
				State->Fail(TEXT("Resolve game version"), FFlockError::Make(EFlockErrorType::Validation,
					FString::Printf(TEXT("Server returned no id for game version '%s'."), *GameVersionName)));
				return;
			}

			State->Snapshot.GameVersionId = Result.Value.Id;
			State->Headers.Add(TEXT("X-Game-Version-ID"), Result.Value.Id);
			FetchTemplates(State);
		});
}

void FFlockSchemaFetcher::FetchFromSettings(FOnSchemaFetched OnComplete)
{
	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	if (!Config)
	{
		OnComplete.ExecuteIfBound(TFlockResult<FFlockSchemaSnapshot>::Fail(FFlockError::Make(
			EFlockErrorType::Validation, TEXT("Flock settings are unavailable."))));
		return;
	}

	// Checked individually rather than through the shared validator so the message names the field that
	// is actually missing — this runs from a menu action, where the fix should be obvious.
	FString Missing;
	if (Config->ApiUrl.IsEmpty())
	{
		Missing = TEXT("API Url");
	}
	else if (Config->ApiKey.IsEmpty())
	{
		Missing = TEXT("API Key");
	}
	else if (Config->GameVersion.IsEmpty())
	{
		Missing = TEXT("Game Version");
	}
	if (!Missing.IsEmpty())
	{
		OnComplete.ExecuteIfBound(TFlockResult<FFlockSchemaSnapshot>::Fail(FFlockError::Make(
			EFlockErrorType::Validation,
			FString::Printf(TEXT("%s is not set in Project Settings > Flock SDK."), *Missing))));
		return;
	}

	Fetch(CreateDefaultClient(), Config->ApiUrl, Config->ApiKey, Config->GameVersion, Config->GameVersionId, OnComplete);
}

TSharedRef<FFlockHttpClient> FFlockSchemaFetcher::CreateDefaultClient()
{
	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	const float Timeout = Config ? Config->HttpTimeoutSeconds : 30.f;
	const bool bVerbose = Config ? Config->bEnableDebugLogs : false;
	return FFlockHttpClient::CreateDefault(Timeout, MakeShared<FFlockUnrealLogger>(bVerbose));
}
