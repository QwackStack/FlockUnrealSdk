// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Auth/FlockAuthSession.h"
#include "FlockEvents.h"
#include "Http/FlockEndpoints.h"
#include "Http/FlockHttpClient.h"
#include "Misc/ScopeExit.h"
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
	// A sign-in answered after Flock shut down belongs to no initialization: the saved sign-in is the next one's.
	if (bShutDown)
	{
		OutError = TEXT("Flock was shut down before the server answered this sign-in, so it was not kept");
		return false;
	}
	TOptional<FFlockJwtClaims> NewClaims;
	if (!InAccessToken.IsEmpty())
	{
		FFlockJwtClaims Parsed;
		FString ParseError;
		if (!FFlockJwt::Parse(InAccessToken, Parsed, ParseError))
		{
			Logger->LogError(FString::Printf(TEXT("Failed to parse JWT access token: %s"), *ParseError));
			OutError = TEXT("The server's sign-in answer holds an access token that cannot be read, so no player session was made");
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
	ForgetTokensInMemory();
	if (Store.IsValid())
	{
		Store->Clear();
	}
}

void FFlockAuthSession::EndSignInForShutdown()
{
	Logger->LogDebug(TEXT("Letting go of this initialization's tokens; the saved sign-in stays"));
	ForgetTokensInMemory();
	bShutDown = true;
}

void FFlockAuthSession::BecomeTheRunningSession()
{
	RunningSession() = AsShared();
}

void FFlockAuthSession::ForgetTokensInMemory()
{
	++SignInNumber;
	AccessToken.Empty();
	RefreshToken.Empty();
	Claims.Reset();
	AuthMethod.Reset();
}

TWeakPtr<FFlockAuthSession>& FFlockAuthSession::RunningSession()
{
	static TWeakPtr<FFlockAuthSession> Running;
	return Running;
}

TMap<FString, FFlockAuthSession::FRefreshOnItsWay>& FFlockAuthSession::RefreshesOnTheirWay()
{
	static TMap<FString, FRefreshOnItsWay> OnTheirWay;
	return OnTheirWay;
}

void FFlockAuthSession::LetTheWaitersCarryOn(const FString& SentRefreshToken)
{
	FRefreshOnItsWay OnItsWay;
	if (RefreshesOnTheirWay().RemoveAndCopyValue(SentRefreshToken, OnItsWay))
	{
		for (const TFunction<void()>& Waiter : OnItsWay.Waiters)
		{
			Waiter();
		}
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

	// An earlier initialization sent this very refresh token before Flock shut down, and has not heard back. A second
	// request with it would race the first, and the server keeps one answer only: wait, then carry on with what that answer
	// left in the saved sign-in.
	FRefreshOnItsWay* OnItsWay = RefreshesOnTheirWay().Find(RefreshToken);
	if (OnItsWay != nullptr && OnItsWay->Refresh.IsValid() && OnItsWay->SentBy.IsValid() && !OnItsWay->SentBy.HasSameObject(this))
	{
		Refresh->PlayerId = GetPlayerId();
		Refresh->SentRefreshToken = RefreshToken;
		const TSharedRef<FFlockAuthSession> Self = AsShared();
		OnItsWay->Waiters.Add([Self, Refresh]() { Self->CarryOnAfterTheRefreshOnItsWay(Refresh); });
		Logger->LogDebug(TEXT("Waiting for the renewal of this sign-in an earlier initialization sent"));
		return;
	}
	SendRefresh(Refresh);
}

void FFlockAuthSession::CarryOnAfterTheRefreshOnItsWay(const TSharedRef<FRefreshInFlight>& Refresh)
{
	if (Refresh->SignInNumber != SignInNumber)
	{
		FinishRefresh(Refresh, false);
		return;
	}
	if (!TakeANewerSavedSignIn(Refresh))
	{
		SendRefresh(Refresh);
	}
}

void FFlockAuthSession::SendRefresh(const TSharedRef<FRefreshInFlight>& Refresh)
{
	Refresh->PlayerId = GetPlayerId();
	Refresh->SentRefreshToken = RefreshToken;

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
	FRefreshOnItsWay& OnItsWay = RefreshesOnTheirWay().FindOrAdd(Refresh->SentRefreshToken);
	OnItsWay.Refresh = Refresh;
	OnItsWay.SentBy = Self;
	Client->PostJsonRaw<FFlockPlayerLoginResponse>(
		FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, FlockEndpoints::PlayerTokenRefresh),
		BaseHeaders, Json,
		[Self, Refresh, Sent = Refresh->SentRefreshToken](TFlockResult<FFlockPlayerLoginResponse> Result)
		{
			// Whoever waits on this refresh token carries on once this answer has been dealt with.
			ON_SCOPE_EXIT
			{
				LetTheWaitersCarryOn(Sent);
			};

			// A sign-out, another sign-in or a shut-down came first: the answer belongs to a sign-in that is over, so it
			// keeps, clears and raises nothing here. Its new pair is still the only one the server takes for that player.
			if (Refresh->SignInNumber != Self->SignInNumber)
			{
				Self->Logger->LogDebug(TEXT("Token refresh answered after the sign-in it was for had ended; the answer is not used"));
				if (Result.bSuccess && !Result.Value.AccessToken.IsEmpty())
				{
					Self->SaveNewPairWhereTheSentOneIsStillSaved(*Refresh, Result.Value.AccessToken, Result.Value.RefreshToken);
				}
				Self->FinishRefresh(Refresh, false);
				return;
			}

			if (!Result.bSuccess)
			{
				// Only the server refusing the token ends the sign-in. Anything else keeps it, so a flaky network does not
				// sign the player out, and neither does a captive portal's page answering in the server's place (a 200
				// that is not the server's JSON).
				if (Result.Error.Type == EFlockErrorType::Auth)
				{
					// Taken once: the pair taken is the one sent next, so a refusal of it finds nothing newer.
					if (Self->TakeANewerSavedSignIn(Refresh))
					{
						return;
					}
					Self->Logger->LogWarning(TEXT("Token refresh failed: session expired"));
					Self->FailRefreshAsExpired(Refresh);
				}
				else
				{
					Self->Logger->LogError(FString::Printf(TEXT("Renewing the sign-in failed: %s"), *Result.Error.ReasonAndFix()));
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

void FFlockAuthSession::SaveNewPairWhereTheSentOneIsStillSaved(const FRefreshInFlight& Refresh, const FString& NewAccessToken,
	const FString& NewRefreshToken) const
{
	// Through the running initialization's store when Flock was initialized again since: the saved file is the same, and
	// only that store reads it with the running Game Name (a store that cannot read the file deletes it).
	const TSharedPtr<FFlockAuthSession> Running = RunningSession().Pin();
	const TSharedPtr<IFlockTokenStore> SavedIn = Running.IsValid() && !Running->bShutDown ? Running->Store : Store;
	// The saved sign-in moved on (a sign-out cleared it, another sign-in or a renewal replaced it): that one is current.
	FFlockStoredTokens Saved;
	if (Refresh.SentRefreshToken.IsEmpty() || !LoadSavedSignInOf(SavedIn, Refresh.PlayerId, Saved)
		|| !Saved.RefreshToken.Equals(Refresh.SentRefreshToken, ESearchCase::CaseSensitive))
	{
		return;
	}
	Saved.AccessToken = NewAccessToken;
	Saved.RefreshToken = NewRefreshToken;
	SavedIn->Save(Saved);
	Logger->LogDebug(TEXT("The renewed sign-in was saved for the next initialization, as the server no longer takes the one it replaced"));
}

bool FFlockAuthSession::TakeANewerSavedSignIn(const TSharedRef<FRefreshInFlight>& Refresh)
{
	FFlockStoredTokens Saved;
	if (!LoadSavedSignInOf(Store, Refresh->PlayerId, Saved) || Saved.RefreshToken.IsEmpty()
		|| Saved.RefreshToken.Equals(Refresh->SentRefreshToken, ESearchCase::CaseSensitive))
	{
		return false;
	}
	FString AdoptError;
	if (!AdoptTokens(Saved.AccessToken, Saved.RefreshToken, AdoptError))
	{
		return false;
	}
	Logger->LogInfo(TEXT("Carrying on with the player's sign-in as renewed elsewhere (another initialization or another run of the game)"));
	// The same sign-in carries on. A saved access token that has run out is renewed once more, with the saved refresh token.
	if (IsTokenExpired())
	{
		SendRefresh(Refresh);
		return true;
	}
	if (UFlockEvents* Hub = Events.Get())
	{
		Hub->InvokeTokenRefreshed();
	}
	FinishRefresh(Refresh, true);
	return true;
}

bool FFlockAuthSession::LoadSavedSignInOf(const TSharedPtr<IFlockTokenStore>& From, const FString& PlayerId, FFlockStoredTokens& OutSaved)
{
	FFlockJwtClaims SavedClaims;
	FString ParseError;
	return !PlayerId.IsEmpty() && From.IsValid() && From->Load(OutSaved)
		&& FFlockJwt::Parse(OutSaved.AccessToken, SavedClaims, ParseError)
		&& SavedClaims.PlayerId.Equals(PlayerId, ESearchCase::CaseSensitive);
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
	if (BeforeExpiredSignInEnds)
	{
		BeforeExpiredSignInEnds();
	}
	ClearTokens();
	if (UFlockEvents* Hub = Events.Get())
	{
		Hub->InvokeAuthExpired();
	}
	FinishRefresh(Refresh, false);
}
