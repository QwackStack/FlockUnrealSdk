// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockConnectionProbe.h"
#include "Version/FlockVersionLookup.h"
#include "Version/FlockVersionResolver.h"
#include "Models/FlockGameModels.h"
#include "Models/FlockShopModels.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Config/FlockConfig.h"
#include "FlockLogger.h"

EFlockProbeState FFlockConnectionProbe::Classify(const FFlockError& Error)
{
	// No HTTP response at all — DNS, refused, or offline. "Check the API URL" is fair advice here.
	if (Error.Type == EFlockErrorType::Connection)
	{
		return EFlockProbeState::Unreachable;
	}

	// By the server's code, never the status alone: a wrong API URL answers 404 (request.not_found) and a server that
	// is not Flock's game API answers 401 without a code (measured 2026-10-15), and neither is the key or the version.
	switch (Error.ErrorCode)
	{
	case EFlockErrorCode::GameInvalidApiKey:
	case EFlockErrorCode::GameMissingApiKey:
	case EFlockErrorCode::GameGameNotFound:
		return EFlockProbeState::KeyRejected;
	case EFlockErrorCode::GameVersionGameVersionByNameNotFound:
	case EFlockErrorCode::GameVersionGameVersionNotFound:
		return EFlockProbeState::VersionNotFound;
	default:
		break;
	}

	// Everything else — including a timeout — reports the error's own reason and fix rather than a guess. A
	// timeout on a correct URL is common under load, and calling it Unreachable would send the developer
	// to edit a setting that is fine.
	return EFlockProbeState::Failed;
}

bool FFlockConnectionProbe::SellsRealMoney(const TArray<FFlockShop>& Shops)
{
	for (const FFlockShop& Shop : Shops)
	{
		for (const FFlockShopItem& Item : Shop.ShopItems)
		{
			if (Item.IsRealMoney)
			{
				return true;
			}
		}
	}
	return false;
}

namespace
{
	constexpr int32 ShopPageSize = 100;

	/** Step 3: reads the shops a page at a time until a real-money item or the last page; unset when a page failed. */
	void ReadShopsFrom(const TSharedRef<FFlockHttpClient>& Client, const FString& ApiUrl, const TMap<FString, FString>& Headers, int32 Page,
		TFunction<void(TOptional<bool>)> OnDone)
	{
		const FString Url = FFlockVersionResolver::VersionedUrl(ApiUrl, FString::Printf(TEXT("shop?page=%d&limit=%d"), Page, ShopPageSize));
		Client->GetPaged<FFlockShop>(Url, Headers,
			[Client, ApiUrl, Headers, Page, OnDone](TFlockResult<TFlockPage<FFlockShop>> Result)
			{
				if (!Result.bSuccess)
				{
					OnDone(TOptional<bool>());
					return;
				}
				if (FFlockConnectionProbe::SellsRealMoney(Result.Value.Items))
				{
					OnDone(true);
					return;
				}
				if (Result.Value.Items.Num() == 0 || Page * ShopPageSize >= Result.Value.Total)
				{
					OnDone(false);
					return;
				}
				ReadShopsFrom(Client, ApiUrl, Headers, Page + 1, OnDone);
			});
	}
}

void FFlockConnectionProbe::Run(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion,
	FFlockProbeComplete OnComplete)
{
	// Step 1 goes through the registered lookup, so the probe honours the same seam the resolve does and
	// stays fake-able in tests.
	FFlockVersionLookupRegistry::Get().Resolve(ApiUrl, ApiKey, GameVersion,
		FFlockResolveComplete::CreateLambda([ApiUrl, ApiKey, OnComplete](const FFlockResolveResult& Resolve)
		{
			if (!Resolve.bSuccess)
			{
				FFlockProbeResult Result;
				Result.State = Classify(Resolve.Detail);
				Result.Message = Resolve.Detail.ReasonAndFix();
				OnComplete.ExecuteIfBound(Result);
				return;
			}

			// Step 2: the game record, for its name. Needs the version ID step 1 just produced.
			const UFlockConfig* Config = GetDefault<UFlockConfig>();
			const float Timeout = Config ? Config->HttpTimeoutSeconds : 30.f;
			const bool bVerbose = Config ? Config->bEnableDebugLogs : false;

			TMap<FString, FString> Headers;
			Headers.Add(TEXT("X-Flock-API-Key"), ApiKey);
			Headers.Add(TEXT("X-Game-Version-ID"), Resolve.GameVersionId);

			const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockUnrealLogger>(bVerbose);
			const TSharedRef<FFlockHttpClient> Client = FFlockHttpClient::CreateDefault(Timeout, Logger);
			const FString Url = FFlockVersionResolver::VersionedUrl(ApiUrl, TEXT("game"));

			const FString ResolvedId = Resolve.GameVersionId;
			Client->Get<FFlockGameSchema>(Url, Headers,
				[OnComplete, ResolvedId, Client, ApiUrl, Headers](TFlockResult<FFlockGameSchema> Game)
				{
					FFlockProbeResult Result;
					Result.GameVersionId = ResolvedId;

					// Step 1 already proved URL, key and version name. A step 2 failure says nothing about
					// those, so it must not downgrade a connection that demonstrably works — the probe
					// still reports Ok, just without a game name to compare.
					Result.State = EFlockProbeState::Ok;
					if (Game.bSuccess)
					{
						Result.ServerGameName = Game.Value.Name;
					}

					ReadShopsFrom(Client, ApiUrl, Headers, 1, [OnComplete, Result](TOptional<bool> SellsRealMoney) mutable
					{
						Result.SellsRealMoney = SellsRealMoney;
						OnComplete.ExecuteIfBound(Result);
					});
				});
		}));
}
