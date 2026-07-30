// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Setup/FlockConnectionProbe.h"
#include "Version/FlockVersionLookup.h"
#include "Version/FlockVersionResolver.h"
#include "Models/FlockGameModels.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockResult.h"
#include "Config/FlockConfig.h"
#include "FlockLogger.h"

EFlockProbeState FFlockConnectionProbe::Classify(const FFlockError& Error)
{
	switch (Error.Type)
	{
	case EFlockErrorType::Connection:
		// No HTTP response at all — DNS, refused, or offline. "Check the API URL" is fair advice here.
		return EFlockProbeState::Unreachable;

	case EFlockErrorType::Auth:
		return EFlockProbeState::KeyRejected;

	default:
		break;
	}

	// 401/403 without the Auth classification still mean the key was refused.
	if (Error.StatusCode == 401 || Error.StatusCode == 403)
	{
		return EFlockProbeState::KeyRejected;
	}

	// The only 404 this route can produce: the version name did not match. The game is identified by the
	// API key and is not part of the request, so a 404 can never mean "no such game".
	if (Error.StatusCode == 404)
	{
		return EFlockProbeState::VersionNotFound;
	}

	// Everything else — including a timeout — reports the server's own wording rather than a guess. A
	// timeout on a correct URL is common under load, and calling it Unreachable would send the developer
	// to edit a setting that is fine.
	return EFlockProbeState::Failed;
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
				Result.Message = Resolve.Detail.ServerMessage.IsEmpty() ? Resolve.Error : Resolve.Detail.ServerMessage;
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
				[OnComplete, ResolvedId](TFlockResult<FFlockGameSchema> Game)
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

					OnComplete.ExecuteIfBound(Result);
				});
		}));
}
