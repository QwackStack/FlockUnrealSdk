// Copyright 2022, Qwacks. All Rights Reserved.

#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockHttpClient.h"
#include "Models/FlockAuthModels.h"

FFlockAuthSession::FFlockAuthSession(const TSharedRef<FFlockHttpClient>& InClient,
	const TSharedPtr<IFlockTokenStore>& InStore, const TSharedRef<IFlockLogger>& InLogger,
	const FString& InVersionedApiUrl, const TMap<FString, FString>& InBaseHeaders)
	: Client(InClient)
	, Store(InStore)
	, Logger(InLogger)
	, VersionedApiUrl(InVersionedApiUrl)
	, BaseHeaders(InBaseHeaders)
{
}

bool FFlockAuthSession::SetTokens(const FString& InAccessToken, const FString& InRefreshToken, FString& OutError)
{
	TOptional<FFlockJwtClaims> NewClaims;
	if (!InAccessToken.IsEmpty())
	{
		FFlockJwtClaims Parsed;
		FString ParseError;
		if (!FFlockJwt::Parse(InAccessToken, Parsed, ParseError))
		{
			Logger->LogError(FString::Printf(TEXT("Failed to parse JWT access token: %s"), *ParseError));
			OutError = TEXT("Server returned an unparseable JWT access token. Cannot establish player session — ")
				TEXT("the auth response was malformed. Verify ApiUrl points at a supported Flock backend.");
			return false;
		}
		NewClaims = MoveTemp(Parsed);
	}

	AccessToken = InAccessToken;
	RefreshToken = InRefreshToken;
	Claims = MoveTemp(NewClaims);

	if (Claims.IsSet())
	{
		Logger->LogDebug(FString::Printf(TEXT("Token set for PlayerId: %s"), *Claims.GetValue().PlayerId));
	}

	Persist();
	return true;
}

void FFlockAuthSession::SetAuthMethod(EFlockAuthMethod Method)
{
	AuthMethod = Method;
	Persist();
}

void FFlockAuthSession::ClearTokens()
{
	Logger->LogInfo(TEXT("Clearing authentication tokens"));
	AccessToken.Empty();
	RefreshToken.Empty();
	Claims.Reset();
	AuthMethod.Reset();
	if (Store.IsValid())
	{
		Store->Clear();
	}
}

bool FFlockAuthSession::IsTokenExpired() const
{
	return Claims.IsSet() && Claims.GetValue().ExpirationTime.IsSet()
		&& Claims.GetValue().ExpirationTime.GetValue() <= FDateTime::UtcNow();
}

TMap<FString, FString> FFlockAuthSession::GetAuthHeaders() const
{
	TMap<FString, FString> Headers = BaseHeaders;
	if (!AccessToken.IsEmpty())
	{
		Headers.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	}
	return Headers;
}

bool FFlockAuthSession::LoadPersisted(FFlockStoredTokens& OutTokens) const
{
	if (!Store.IsValid())
	{
		return false;
	}
	const bool bLoaded = Store->Load(OutTokens);
	Logger->LogDebug(bLoaded ? TEXT("Token store: tokens restored") : TEXT("Token store: no stored session"));
	return bLoaded;
}

void FFlockAuthSession::Persist()
{
	if (!Store.IsValid())
	{
		return;
	}
	FFlockStoredTokens Tokens;
	Tokens.AccessToken = AccessToken;
	Tokens.RefreshToken = RefreshToken;
	Tokens.AuthMethod = AuthMethod;
	Store->Save(Tokens); // empty access token behaves as Clear inside the store
}

void FFlockAuthSession::RefreshAccessToken(TFunction<void(bool)> OnComplete)
{
	if (RefreshToken.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(false);
		}
		return;
	}

	PendingRefreshCallbacks.Add(MoveTemp(OnComplete));
	if (bRefreshInFlight)
	{
		// Piggyback: the queued callback completes with the in-flight POST's outcome.
		return;
	}
	bRefreshInFlight = true;

	FFlockPlayerRefreshTokenRequest Request;
	Request.PlayerId = GetPlayerId();
	Request.RefreshToken = RefreshToken;

	FString Json;
	if (!FFlockJsonUtils::StructToWireJson(Request, Json, /*bOmitEmptyStrings*/ true))
	{
		DrainRefreshCallbacks(false);
		return;
	}

	Logger->LogDebug(FString::Printf(TEXT("Refresh POST %s/%s"), *VersionedApiUrl, FlockEndpoints::PlayerTokenRefresh));

	// Base headers only: the expired bearer must not ride along. One attempt, no retry handler —
	// callers own their retry semantics, and a second POST could invalidate a rotated token.
	// Raw (non-enveloped): the token routes return the model at the root.
	const TSharedRef<FFlockAuthSession> Self = AsShared();
	Client->PostJsonRaw<FFlockPlayerLoginResponse>(
		FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, FlockEndpoints::PlayerTokenRefresh),
		BaseHeaders, Json,
		[Self](TFlockResult<FFlockPlayerLoginResponse> Result)
		{
			if (!Result.bSuccess)
			{
				// An authoritative rejection kills the session; transport-shaped failures keep it
				// so a flaky network doesn't sign the player out.
				const bool bAuthoritative = Result.Error.Type == EFlockErrorType::Auth
					|| Result.Error.Type == EFlockErrorType::Serialization;
				if (bAuthoritative)
				{
					Self->Logger->LogWarning(TEXT("Token refresh failed: session expired"));
					Self->FailRefreshAsExpired();
				}
				else
				{
					Self->Logger->LogError(FString::Printf(TEXT("Token refresh failed: %s"), *Result.Error.Message));
					Self->DrainRefreshCallbacks(false);
				}
				return;
			}

			if (Result.Value.AccessToken.IsEmpty())
			{
				Self->Logger->LogWarning(TEXT("Token refresh failed: empty access token from server"));
				Self->FailRefreshAsExpired();
				return;
			}

			FString SetError;
			if (!Self->SetTokens(Result.Value.AccessToken, Result.Value.RefreshToken, SetError))
			{
				Self->Logger->LogWarning(FString::Printf(TEXT("Token refresh failed: %s"), *SetError));
				Self->FailRefreshAsExpired();
				return;
			}

			Self->Logger->LogInfo(TEXT("Token refresh successful"));
			if (UFlockEvents* Hub = Self->Events.Get())
			{
				Hub->InvokeTokenRefreshed();
			}
			Self->DrainRefreshCallbacks(true);
		});
}

void FFlockAuthSession::DrainRefreshCallbacks(bool bSuccess)
{
	bRefreshInFlight = false;
	TArray<TFunction<void(bool)>> Callbacks = MoveTemp(PendingRefreshCallbacks);
	PendingRefreshCallbacks.Reset();
	for (const TFunction<void(bool)>& Callback : Callbacks)
	{
		if (Callback)
		{
			Callback(bSuccess);
		}
	}
}

void FFlockAuthSession::FailRefreshAsExpired()
{
	ClearTokens();
	if (UFlockEvents* Hub = Events.Get())
	{
		Hub->InvokeAuthExpired();
	}
	DrainRefreshCallbacks(false);
}
