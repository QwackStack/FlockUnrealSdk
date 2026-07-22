// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Auth/FlockAuthSession.h"
#include "FlockEventModels.h"
#include "FlockEvents.h"
#include "Http/FlockProviderBase.h"
#include "Models/FlockAuthModels.h"
#include "UObject/WeakObjectPtrTemplates.h"

/**
 * Player authentication: login/register across identity providers, session restore, account
 * flows (password, email verification, token revoke), and logout. Owned by UFlockSubsystem;
 * Blueprint reaches it through the Flock auth async nodes.
 *
 * Login/register success adopts the returned tokens into the auth session, records the method,
 * and raises OnAuthenticated. Auth routes opt out of the base's silent refresh (a credential
 * failure must surface, not trigger a refresh loop); account routes keep it.
 *
 * Completion-lambda rule: capture shared refs/weak object ptrs/values only — never `this` —
 * so teardown with requests in flight stays safe.
 */
class FLOCK_API FFlockAuthProvider : public FFlockProviderBase
{
public:
	FFlockAuthProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
		const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
		const TWeakObjectPtr<UFlockEvents>& InEvents, const FString& InVersionedApiUrl);

	// ── Login ──

	FFlockRequestHandle LoginWithEmail(const FString& Email, const FString& Password,
		TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete);

	FFlockRequestHandle LoginWithDevice(const FString& DeviceId,
		TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete);

	FFlockRequestHandle LoginWithGoogle(const FString& IdToken,
		TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete);

	FFlockRequestHandle LoginWithApple(const FString& IdentityToken,
		TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete);

	FFlockRequestHandle LoginWithSteam(const FString& SessionTicket,
		TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete);

	/** No dedicated backend route — posts to the generic login with the provider id (login only). */
	FFlockRequestHandle LoginWithFacebook(const FString& FacebookId,
		TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete);

	/** No dedicated backend route — posts to the generic login with the provider id (login only). */
	FFlockRequestHandle LoginWithDiscord(const FString& DiscordId,
		TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete);

	// ── Register ──
	// Name is an optional display name (server-enforced unique); pass empty to omit. When the
	// identity is already registered the call completes successfully with bAlreadyRegistered set.

	FFlockRequestHandle RegisterWithEmail(const FString& Email, const FString& Password, const FString& Name,
		TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete);

	FFlockRequestHandle RegisterWithDevice(const FString& DeviceId, const FString& Name,
		TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete);

	FFlockRequestHandle RegisterWithGoogle(const FString& IdToken, const FString& Name,
		TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete);

	FFlockRequestHandle RegisterWithApple(const FString& IdentityToken, const FString& Name,
		TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete);

	FFlockRequestHandle RegisterWithSteam(const FString& SessionTicket, const FString& Name,
		TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete);

	// ── Session ──

	/**
	 * Resumes a persisted session, refreshing an expired token. OnSessionRestored is raised on
	 * every outcome (false included); OnAuthenticated fires only on success, with SessionRestore
	 * as the method while the original login method is re-adopted for method-gated flows.
	 */
	void TryRestoreSession(TFunction<void(bool)> OnComplete);

	/** True while TryRestoreSession is running. */
	bool IsRestoringSession() const { return *bRestoringSession; }

	/**
	 * Clears local authentication state; safe when signed out. Raises OnLoggedOut only when a
	 * player was signed in. Server-side revocation is separate — see RevokeToken.
	 */
	void Logout();

	/** Explicit access-token refresh (session passthrough): false when there is no refresh token or the refresh fails. */
	void RefreshToken(TFunction<void(bool)> OnComplete);

	// ── Account ──

	/** Emails a password-reset code. The backend always reports success (it never reveals whether the email exists). */
	FFlockRequestHandle ForgotPassword(const FString& Email,
		TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete);

	/** Sets a new password using the emailed code. Requires being signed in with email (restored email sessions count). */
	FFlockRequestHandle ResetPassword(const FString& Email, const FString& Code, const FString& NewPassword,
		TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete);

	/** Emails a verification code to the player's address. The bearer token rides along automatically when present. */
	FFlockRequestHandle SendEmailVerification(TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete);

	/** Marks the player's email verified using the code from SendEmailVerification. */
	FFlockRequestHandle VerifyEmail(const FString& Code,
		TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete);

	/**
	 * Revokes the signed-in player's refresh token server-side (logout hardening / killing a stolen
	 * token). Issued access tokens live out their TTL; local session is untouched — call Logout
	 * after for a full sign-out.
	 */
	FFlockRequestHandle RevokeToken(TFunction<void(TFlockResult<FFlockTokenRevokeResponse>)> OnComplete);

	/** Registration preflight: whether a display name is still free. Advisory — a race can still lose at register time. */
	FFlockRequestHandle IsNameAvailable(const FString& Name,
		TFunction<void(TFlockResult<FFlockNameAvailableResponse>)> OnComplete);

private:
	/**
	 * Shared login/register success path: validate, adopt tokens, record method, raise OnAuthenticated.
	 * IsExpectedFailure lets the registration path declare "already registered" as a normal outcome so
	 * it is not logged as an error on the way to being converted into a success.
	 */
	template <typename TReq>
	FFlockRequestHandle ExecuteAuth(const TReq& Request, const FString& Endpoint, const FString& Context,
		EFlockAuthMethod Method, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete,
		TFunction<bool(const FFlockError&)> IsExpectedFailure = nullptr);

	/** ExecuteAuth + the already-registered short-circuit into a successful FFlockRegisterResult. */
	template <typename TReq>
	FFlockRequestHandle ExecuteRegistration(const TReq& Request, const FString& Endpoint, const FString& Context,
		EFlockAuthMethod Method, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete);

	/** Fails fast with an Auth error when no player is signed in. */
	template <typename T>
	bool RequireAuthenticated(const TFunction<void(TFlockResult<T>)>& OnComplete) const;

	/** Fails fast unless signed in via email (restored email sessions count). */
	template <typename T>
	bool RequireEmailAuth(const TFunction<void(TFlockResult<T>)>& OnComplete) const;

	/** Shared account-flow POST: serialize (omitting empty optionals), per-attempt auth headers, run under Execute. */
	template <typename TReq, typename TResp>
	FFlockRequestHandle PostAccount(const TReq& Request, const FString& Endpoint, const FString& Context,
		TFunction<void(TFlockResult<TResp>)> OnComplete, bool bIdempotent = false);

	FString AuthUrl(const FString& Endpoint) const;

	TSharedRef<FFlockAuthSession> Session;
	TWeakObjectPtr<UFlockEvents> Events;
	FString VersionedApiUrl;

	/** Heap-shared (not a plain member) so async restore legs can clear it even if the provider is torn down mid-restore. */
	TSharedRef<bool> bRestoringSession = MakeShared<bool>(false);
};

// ── Template implementations ──

template <typename TReq>
FFlockRequestHandle FFlockAuthProvider::ExecuteAuth(const TReq& Request, const FString& Endpoint,
	const FString& Context, EFlockAuthMethod Method, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete,
	TFunction<bool(const FFlockError&)> IsExpectedFailure)
{
	FString Json;
	if (!FFlockJsonUtils::StructToWireJson(Request, Json, /*bOmitEmptyStrings*/ true))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<FFlockPlayerLoginResponse>::Fail(
				FFlockError::Make(EFlockErrorType::Serialization, TEXT("Failed to serialize request body"))));
		}
		return FFlockRequestHandle();
	}

	// Decorated once at dispatch so the async completion still names the caller (C++ or a Blueprint).
	// The raw Context stays in caller-facing error messages — the origin belongs in logs, not errors.
	const FString LoggedContext = DecorateContext(Context);
	Logger->LogInfo(FString::Printf(TEXT("%s starting..."), *LoggedContext));

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = AuthUrl(Endpoint);
	FFlockRetryHandler::FOperation<FFlockPlayerLoginResponse> Operation =
		[ClientRef, SessionRef, Url, Json](TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnAttempt)
		{
			// Headers fetched per attempt: a signed-in re-login carries the current bearer.
			// Raw (non-enveloped): the player auth routes return the model at the root.
			return ClientRef->PostJsonRaw<FFlockPlayerLoginResponse>(Url, SessionRef->GetAuthHeaders(), Json, MoveTemp(OnAttempt));
		};

	const TWeakObjectPtr<UFlockEvents> EventsWeak = Events;
	const TSharedRef<IFlockLogger> Log = Logger;
	return Execute<FFlockPlayerLoginResponse>(MoveTemp(Operation),
		[SessionRef, EventsWeak, Log, Context, LoggedContext, Method, OnComplete](TFlockResult<FFlockPlayerLoginResponse> Result)
		{
			if (!Result.bSuccess)
			{
				if (OnComplete)
				{
					OnComplete(Result);
				}
				return;
			}

			if (Result.Value.AccessToken.IsEmpty())
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerLoginResponse>::Fail(FFlockError::Make(EFlockErrorType::Auth,
						FString::Printf(TEXT("Invalid %s response from server"), *Context.ToLower()))));
				}
				return;
			}

			FString SetError;
			if (!SessionRef->SetTokens(Result.Value.AccessToken, Result.Value.RefreshToken, SetError))
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockPlayerLoginResponse>::Fail(
						FFlockError::Make(EFlockErrorType::Auth, SetError)));
				}
				return;
			}

			SessionRef->SetAuthMethod(Method);
			Log->LogInfo(FString::Printf(TEXT("%s successful for player: %s"), *LoggedContext, *SessionRef->GetPlayerId()));

			if (UFlockEvents* Hub = EventsWeak.Get())
			{
				FFlockAuthInfo Info;
				Info.PlayerId = SessionRef->GetPlayerId();
				Info.Method = Method;
				Hub->InvokeAuthenticated(Info);
			}
			// Analytics auto-init hook slots in here when the analytics feature lands.

			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		Context, /*bIdempotent*/ true, /*MaxRetriesOverride*/ -1, /*bAllowAuthRetry*/ false,
		MoveTemp(IsExpectedFailure));
}

template <typename TReq>
FFlockRequestHandle FFlockAuthProvider::ExecuteRegistration(const TReq& Request, const FString& Endpoint,
	const FString& Context, EFlockAuthMethod Method, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
{
	const TSharedRef<IFlockLogger> Log = Logger;
	const FString LoggedContext = DecorateContext(Context);
	return ExecuteAuth(Request, Endpoint, Context, Method,
		[Log, LoggedContext, OnComplete](TFlockResult<FFlockPlayerLoginResponse> Result)
		{
			if (!OnComplete)
			{
				return;
			}
			if (Result.bSuccess)
			{
				FFlockRegisterResult Registered;
				Registered.Response = Result.Value;
				OnComplete(TFlockResult<FFlockRegisterResult>::Ok(Registered));
				return;
			}
			if (Result.Error.IsAlreadyRegistered())
			{
				Log->LogWarning(FString::Printf(TEXT("%s skipped: player already registered."), *LoggedContext));
				FFlockRegisterResult Existing;
				Existing.bAlreadyRegistered = true;
				OnComplete(TFlockResult<FFlockRegisterResult>::Ok(Existing));
				return;
			}
			OnComplete(TFlockResult<FFlockRegisterResult>::Fail(Result.Error));
		},
		// Registering an identity that already exists is the documented success path below, not a
		// failure — so it must not be logged as one on the way there.
		[](const FFlockError& Error) { return Error.IsAlreadyRegistered(); });
}

template <typename T>
bool FFlockAuthProvider::RequireAuthenticated(const TFunction<void(TFlockResult<T>)>& OnComplete) const
{
	if (!Session->IsAuthenticated())
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Auth, TEXT("No player is signed in"))));
		}
		return false;
	}
	return true;
}

template <typename T>
bool FFlockAuthProvider::RequireEmailAuth(const TFunction<void(TFlockResult<T>)>& OnComplete) const
{
	if (!RequireAuthenticated<T>(OnComplete))
	{
		return false;
	}
	if (Session->GetAuthMethod() != EFlockAuthMethod::Email)
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<T>::Fail(FFlockError::Make(EFlockErrorType::Auth,
				TEXT("Password reset requires being signed in with email"))));
		}
		return false;
	}
	return true;
}

template <typename TReq, typename TResp>
FFlockRequestHandle FFlockAuthProvider::PostAccount(const TReq& Request, const FString& Endpoint,
	const FString& Context, TFunction<void(TFlockResult<TResp>)> OnComplete, bool bIdempotent)
{
	FString Json;
	if (!FFlockJsonUtils::StructToWireJson(Request, Json, /*bOmitEmptyStrings*/ true))
	{
		if (OnComplete)
		{
			OnComplete(TFlockResult<TResp>::Fail(
				FFlockError::Make(EFlockErrorType::Serialization, TEXT("Failed to serialize request body"))));
		}
		return FFlockRequestHandle();
	}

	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = AuthUrl(Endpoint);
	return Execute<TResp>(
		[ClientRef, SessionRef, Url, Json](TFunction<void(TFlockResult<TResp>)> OnAttempt)
		{
			// Headers fetched per attempt so a silent-refresh replay carries the new bearer.
			// Raw (non-enveloped): the player auth routes return the model at the root.
			return ClientRef->PostJsonRaw<TResp>(Url, SessionRef->GetAuthHeaders(), Json, MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), Context, bIdempotent);
}
