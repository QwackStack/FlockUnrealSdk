// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

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
	if (!AdoptTokens(InAccessToken, InRefreshToken, OutError))
	{
		return false;
	}
	++SignInNumber;
	return true;
}

bool FFlockAuthSession::AdoptTokens(const FString& InAccessToken, const FString& InRefreshToken, FString& OutError)
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
	++SignInNumber;
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

void FFlockAuthSession::RefreshAccessToken(int32 ForSignInNumber, TFunction<void(bool)> OnComplete)
{
	// The sign-in this refresh was asked for has ended, or there is nothing to refresh with: no request.
	if (ForSignInNumber != SignInNumber || RefreshToken.IsEmpty())
	{
		if (OnComplete)
		{
			OnComplete(false);
		}
		return;
	}

	// Piggyback on a refresh already on its way for this sign-in; one sent for an earlier sign-in is not this one's.
	if (RefreshInFlight.IsValid() && RefreshInFlight->SignInNumber == SignInNumber)
	{
		RefreshInFlight->Callbacks.Add(MoveTemp(OnComplete));
		return;
	}
	const TSharedRef<FRefreshInFlight> Refresh = MakeShared<FRefreshInFlight>();
	Refresh->SignInNumber = SignInNumber;
	Refresh->Callbacks.Add(MoveTemp(OnComplete));
	RefreshInFlight = Refresh;

	FFlockPlayerRefreshTokenRequest Request;
	Request.PlayerId = GetPlayerId();
	Request.RefreshToken = RefreshToken;

	FString Json;
	if (!FFlockJsonUtils::StructToWireJson(Request, Json, /*bOmitEmptyStrings*/ true))
	{
		FinishRefresh(Refresh, false);
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
		[Self, Refresh](TFlockResult<FFlockPlayerLoginResponse> Result)
		{
			// A sign-out or another sign-in came first: the answer belongs to a sign-in that is over, so it keeps,
			// clears and raises nothing.
			if (Refresh->SignInNumber != Self->SignInNumber)
			{
				Self->Logger->LogDebug(TEXT("Token refresh answered after the sign-in it was for had ended; the answer is not used"));
				Self->FinishRefresh(Refresh, false);
				return;
			}

			if (!Result.bSuccess)
			{
				// An authoritative rejection kills the session; transport-shaped failures keep it
				// so a flaky network doesn't sign the player out.
				const bool bAuthoritative = Result.Error.Type == EFlockErrorType::Auth
					|| Result.Error.Type == EFlockErrorType::Serialization;
				if (bAuthoritative)
				{
					Self->Logger->LogWarning(TEXT("Token refresh failed: session expired"));
					Self->FailRefreshAsExpired(Refresh);
				}
				else
				{
					Self->Logger->LogError(FString::Printf(TEXT("Token refresh failed: %s"), *Result.Error.Message));
					Self->FinishRefresh(Refresh, false);
				}
				return;
			}

			if (Result.Value.AccessToken.IsEmpty())
			{
				Self->Logger->LogWarning(TEXT("Token refresh failed: empty access token from server"));
				Self->FailRefreshAsExpired(Refresh);
				return;
			}

			// The same sign-in carries on with new tokens, so its number stays.
			FString SetError;
			if (!Self->AdoptTokens(Result.Value.AccessToken, Result.Value.RefreshToken, SetError))
			{
				Self->Logger->LogWarning(FString::Printf(TEXT("Token refresh failed: %s"), *SetError));
				Self->FailRefreshAsExpired(Refresh);
				return;
			}

			Self->Logger->LogInfo(TEXT("Token refresh successful"));
			if (UFlockEvents* Hub = Self->Events.Get())
			{
				Hub->InvokeTokenRefreshed();
			}
			Self->FinishRefresh(Refresh, true);
		});
}

void FFlockAuthSession::FinishRefresh(const TSharedRef<FRefreshInFlight>& Refresh, bool bSuccess)
{
	if (RefreshInFlight == Refresh)
	{
		RefreshInFlight.Reset();
	}
	TArray<TFunction<void(bool)>> Callbacks = MoveTemp(Refresh->Callbacks);
	Refresh->Callbacks.Reset();
	for (const TFunction<void(bool)>& Callback : Callbacks)
	{
		if (Callback)
		{
			Callback(bSuccess);
		}
	}
}

void FFlockAuthSession::FailRefreshAsExpired(const TSharedRef<FRefreshInFlight>& Refresh)
{
	ClearTokens();
	if (UFlockEvents* Hub = Events.Get())
	{
		Hub->InvokeAuthExpired();
	}
	FinishRefresh(Refresh, false);
}
