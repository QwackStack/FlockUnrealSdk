// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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
				// Carry the typed error, not just its message: the connection probe needs the error type
				// and HTTP status to say *which* credential was refused.
				OnComplete.ExecuteIfBound(FFlockResolveResult::FailWith(Result.Error));
				return;
			}
			if (Result.Value.Id.IsEmpty())
			{
				OnComplete.ExecuteIfBound(FFlockResolveResult::FailWith(FFlockError::MakeWithFix(EFlockErrorType::Serialization, TEXT("The server returned no Game Version ID"), TEXT("Check the Game Version name matches one in the Flock dashboard, and that API URL under Project Settings > Flock SDK points at the server the game lives on."))));
				return;
			}
			OnComplete.ExecuteIfBound(FFlockResolveResult::Ok(Result.Value.Id));
		});
}
