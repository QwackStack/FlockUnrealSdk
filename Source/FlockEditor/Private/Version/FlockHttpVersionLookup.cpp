// Copyright 2022, Qwacks. All Rights Reserved.

#include "Version/FlockHttpVersionLookup.h"
#include "Version/FlockVersionResolver.h"
#include "Models/FlockGameModels.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Config/FlockConfig.h"
#include "FlockLogger.h"

void FFlockHttpVersionLookup::Resolve(const FString& ApiUrl, const FString& ApiKey, const FString& GameVersion, FFlockResolveComplete OnComplete)
{
	// Reuse the resolver's URL builder so this stays byte-identical to any connection test.
	const FString Url = FFlockVersionResolver::ByNameUrl(ApiUrl, GameVersion);

	// At resolve time the Game Version ID isn't known yet, so only the API key header is sent.
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("X-Flock-API-Key"), ApiKey);

	const UFlockConfig* Config = GetDefault<UFlockConfig>();
	const float Timeout = Config ? Config->HttpTimeoutSeconds : 30.f;
	const bool bVerbose = Config ? Config->bEnableDebugLogs : false;

	const TSharedRef<IFlockLogger> Logger = MakeShared<FFlockUnrealLogger>(bVerbose);
	const TSharedRef<FFlockHttpClient> Client = FFlockHttpClient::CreateDefault(Timeout, Logger);

	// Single attempt — resolve is a manual editor action with clear failure messaging. The in-flight
	// request keeps Client alive internally (Get captures AsShared), so the local ref expiring is fine.
	Client->Get<FFlockGameVersionSchema>(Url, Headers,
		[OnComplete](TFlockResult<FFlockGameVersionSchema> Result)
		{
			if (!Result.bSuccess)
			{
				OnComplete.ExecuteIfBound(FFlockResolveResult::Fail(Result.Error.Message));
				return;
			}
			if (Result.Value.Id.IsEmpty())
			{
				OnComplete.ExecuteIfBound(FFlockResolveResult::Fail(TEXT("Server returned no Game Version ID.")));
				return;
			}
			OnComplete.ExecuteIfBound(FFlockResolveResult::Ok(Result.Value.Id));
		});
}
