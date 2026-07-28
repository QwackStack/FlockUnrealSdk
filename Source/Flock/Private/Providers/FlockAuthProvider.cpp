// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Providers/FlockAuthProvider.h"
#include "FlockEvents.h"
#include "GenericPlatform/GenericPlatformProperties.h"
#include "Http/FlockEndpoints.h"

FFlockAuthProvider::FFlockAuthProvider(const TSharedRef<FFlockHttpClient>& InClient, const FFlockRetryPolicy& InPolicy,
	const TSharedRef<IFlockLogger>& InLogger, const TSharedRef<FFlockAuthSession>& InSession,
	const TWeakObjectPtr<UFlockEvents>& InEvents, const FString& InVersionedApiUrl)
	: FFlockProviderBase(InClient, InPolicy, InLogger)
	, Session(InSession)
	, Events(InEvents)
	, VersionedApiUrl(InVersionedApiUrl)
{
	SetAuthSession(InSession);
}

FString FFlockAuthProvider::AuthUrl(const FString& Endpoint) const
{
	return FString::Printf(TEXT("%s/%s"), *VersionedApiUrl, *Endpoint);
}

// ── Login ──

FFlockRequestHandle FFlockAuthProvider::LoginWithEmail(const FString& Email, const FString& Password,
	TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
{
	FFlockPlayerLoginRequest Request;
	Request.LoginType = TEXT("email");
	Request.Email = Email;
	Request.Password = Password;
	return ExecuteAuth(Request, FlockEndpoints::PlayerLogin, TEXT("Email login"), EFlockAuthMethod::Email, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::LoginWithDevice(const FString& DeviceId,
	TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
{
	FFlockPlayerDeviceLoginRequest Request;
	Request.DeviceType = FPlatformProperties::IniPlatformName();
	Request.DeviceId = DeviceId;
	return ExecuteAuth(Request, FlockEndpoints::PlayerLoginDevice, TEXT("Device login"), EFlockAuthMethod::Device, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::LoginWithGoogle(const FString& IdToken,
	TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
{
	FFlockPlayerGoogleLoginRequest Request;
	Request.IdToken = IdToken;
	return ExecuteAuth(Request, FlockEndpoints::PlayerLoginGoogle, TEXT("Google login"), EFlockAuthMethod::Google, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::LoginWithApple(const FString& IdentityToken,
	TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
{
	FFlockPlayerAppleLoginRequest Request;
	Request.IdentityToken = IdentityToken;
	return ExecuteAuth(Request, FlockEndpoints::PlayerLoginApple, TEXT("Apple login"), EFlockAuthMethod::Apple, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::LoginWithSteam(const FString& SessionTicket,
	TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
{
	FFlockPlayerSteamLoginRequest Request;
	Request.SessionTicket = SessionTicket;
	return ExecuteAuth(Request, FlockEndpoints::PlayerLoginSteam, TEXT("Steam login"), EFlockAuthMethod::Steam, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::LoginWithFacebook(const FString& FacebookId,
	TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
{
	FFlockPlayerLoginRequest Request;
	Request.LoginType = TEXT("facebook");
	Request.FacebookId = FacebookId;
	return ExecuteAuth(Request, FlockEndpoints::PlayerLogin, TEXT("Facebook login"), EFlockAuthMethod::Facebook, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::LoginWithDiscord(const FString& DiscordId,
	TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
{
	FFlockPlayerLoginRequest Request;
	Request.LoginType = TEXT("discord");
	Request.DiscordId = DiscordId;
	return ExecuteAuth(Request, FlockEndpoints::PlayerLogin, TEXT("Discord login"), EFlockAuthMethod::Discord, MoveTemp(OnComplete));
}

// ── Register ──

FFlockRequestHandle FFlockAuthProvider::RegisterWithEmail(const FString& Email, const FString& Password,
	const FString& Name, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
{
	FFlockPlayerEmailRegistrationRequest Request;
	Request.Email = Email;
	Request.Password = Password;
	Request.Name = Name;
	return ExecuteRegistration(Request, FlockEndpoints::PlayerRegister, TEXT("Email registration"),
		EFlockAuthMethod::Email, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::RegisterWithDevice(const FString& DeviceId, const FString& Name,
	TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
{
	FFlockPlayerDeviceRegistrationRequest Request;
	Request.DeviceType = FPlatformProperties::IniPlatformName();
	Request.DeviceId = DeviceId;
	Request.Name = Name;
	return ExecuteRegistration(Request, FlockEndpoints::PlayerRegisterDevice, TEXT("Device registration"),
		EFlockAuthMethod::Device, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::RegisterWithGoogle(const FString& IdToken, const FString& Name,
	TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
{
	FFlockPlayerGoogleRegistrationRequest Request;
	Request.IdToken = IdToken;
	Request.Name = Name;
	return ExecuteRegistration(Request, FlockEndpoints::PlayerRegisterGoogle, TEXT("Google registration"),
		EFlockAuthMethod::Google, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::RegisterWithApple(const FString& IdentityToken, const FString& Name,
	TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
{
	FFlockPlayerAppleRegistrationRequest Request;
	Request.IdentityToken = IdentityToken;
	Request.Name = Name;
	return ExecuteRegistration(Request, FlockEndpoints::PlayerRegisterApple, TEXT("Apple registration"),
		EFlockAuthMethod::Apple, MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::RegisterWithSteam(const FString& SessionTicket, const FString& Name,
	TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
{
	FFlockPlayerSteamRegistrationRequest Request;
	Request.SessionTicket = SessionTicket;
	Request.Name = Name;
	return ExecuteRegistration(Request, FlockEndpoints::PlayerRegisterSteam, TEXT("Steam registration"),
		EFlockAuthMethod::Steam, MoveTemp(OnComplete));
}

// ── Session ──

void FFlockAuthProvider::TryRestoreSession(TFunction<void(bool)> OnComplete)
{
	*bRestoringSession = true;

	// Every capture is shared/weak/value so the async refresh leg stays safe if the provider is
	// torn down mid-restore.
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const TWeakObjectPtr<UFlockEvents> EventsWeak = Events;
	const TSharedRef<IFlockLogger> Log = Logger;
	const TSharedRef<bool> RestoringFlag = bRestoringSession;
	// Restore runs outside Execute (it may never touch the network), so it tags its own logs.
	const FString LoggedContext = DecorateContext(TEXT("Session restore"));
	TFunction<void(bool)> Finish = [RestoringFlag, EventsWeak, OnComplete](bool bRestored)
	{
		*RestoringFlag = false;
		if (UFlockEvents* Hub = EventsWeak.Get())
		{
			Hub->InvokeSessionRestored(bRestored);
		}
		if (OnComplete)
		{
			OnComplete(bRestored);
		}
	};

	FFlockStoredTokens Stored;
	if (!SessionRef->LoadPersisted(Stored) || Stored.AccessToken.IsEmpty())
	{
		Finish(false);
		return;
	}

	FString SetError;
	if (!SessionRef->SetTokens(Stored.AccessToken, Stored.RefreshToken, SetError))
	{
		Log->LogWarning(FString::Printf(TEXT("%s -> stored tokens unusable, clearing them: %s"), *LoggedContext, *SetError));
		SessionRef->ClearTokens();
		Finish(false);
		return;
	}

	// Carry the original login method forward so method-gated flows keep working after a restore.
	SessionRef->SetAuthMethod(Stored.AuthMethod.IsSet() ? Stored.AuthMethod.GetValue() : EFlockAuthMethod::SessionRestore);

	TFunction<void()> CompleteRestored = [SessionRef, EventsWeak, Log, Finish, LoggedContext]()
	{
		Log->LogInfo(FString::Printf(TEXT("%s -> restored PlayerId: %s"), *LoggedContext, *SessionRef->GetPlayerId()));
		if (UFlockEvents* Hub = EventsWeak.Get())
		{
			FFlockAuthInfo Info;
			Info.PlayerId = SessionRef->GetPlayerId();
			Info.Method = EFlockAuthMethod::SessionRestore;
			Hub->InvokeAuthenticated(Info);
		}
		Finish(true);
	};

	if (SessionRef->IsTokenExpired())
	{
		SessionRef->RefreshAccessToken([Finish, CompleteRestored](bool bRefreshed)
		{
			if (!bRefreshed)
			{
				Finish(false);
				return;
			}
			CompleteRestored();
		});
		return;
	}

	CompleteRestored();
}

void FFlockAuthProvider::Logout()
{
	const bool bWasAuthenticated = Session->IsAuthenticated();
	Session->ClearTokens();
	if (bWasAuthenticated)
	{
		if (UFlockEvents* Hub = Events.Get())
		{
			Hub->InvokeLoggedOut();
		}
	}
}

void FFlockAuthProvider::RefreshToken(TFunction<void(bool)> OnComplete)
{
	Session->RefreshAccessToken(MoveTemp(OnComplete));
}

// ── Account ──

FFlockRequestHandle FFlockAuthProvider::ForgotPassword(const FString& Email,
	TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete)
{
	if (!RequireNotEmpty<FFlockAuthActionResponse>(Email, TEXT("Email"), OnComplete))
	{
		return FFlockRequestHandle();
	}
	FFlockPlayerPasswordForgotRequest Request;
	Request.Email = Email;
	return PostAccount<FFlockPlayerPasswordForgotRequest, FFlockAuthActionResponse>(
		Request, FlockEndpoints::PlayerPasswordForgot, TEXT("Password forgot"), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::ResetPassword(const FString& Email, const FString& Code,
	const FString& NewPassword, TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete)
{
	if (!RequireEmailAuth<FFlockAuthActionResponse>(OnComplete)
		|| !RequireNotEmpty<FFlockAuthActionResponse>(Email, TEXT("Email"), OnComplete)
		|| !RequireNotEmpty<FFlockAuthActionResponse>(Code, TEXT("Code"), OnComplete)
		|| !RequireNotEmpty<FFlockAuthActionResponse>(NewPassword, TEXT("NewPassword"), OnComplete))
	{
		return FFlockRequestHandle();
	}
	FFlockPlayerPasswordResetRequest Request;
	Request.Email = Email;
	Request.Code = Code;
	Request.NewPassword = NewPassword;
	return PostAccount<FFlockPlayerPasswordResetRequest, FFlockAuthActionResponse>(
		Request, FlockEndpoints::PlayerPasswordReset, TEXT("Password reset"), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::SendEmailVerification(TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete)
{
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = AuthUrl(FlockEndpoints::PlayerEmailSendVerification);
	return Execute<FFlockAuthActionResponse>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockAuthActionResponse>(Url, SessionRef->GetAuthHeaders(), TEXT("{}"), MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("Send email verification"), /*bIdempotent*/ false);
}

FFlockRequestHandle FFlockAuthProvider::VerifyEmail(const FString& Code,
	TFunction<void(TFlockResult<FFlockAuthActionResponse>)> OnComplete)
{
	if (!RequireNotEmpty<FFlockAuthActionResponse>(Code, TEXT("Code"), OnComplete))
	{
		return FFlockRequestHandle();
	}
	FFlockPlayerEmailVerifyRequest Request;
	Request.Code = Code;
	return PostAccount<FFlockPlayerEmailVerifyRequest, FFlockAuthActionResponse>(
		Request, FlockEndpoints::PlayerEmailVerify, TEXT("Verify email"), MoveTemp(OnComplete));
}

FFlockRequestHandle FFlockAuthProvider::RevokeToken(TFunction<void(TFlockResult<FFlockTokenRevokeResponse>)> OnComplete)
{
	if (!RequireAuthenticated<FFlockTokenRevokeResponse>(OnComplete))
	{
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = AuthUrl(FlockEndpoints::PlayerTokenRevoke);
	return Execute<FFlockTokenRevokeResponse>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockTokenRevokeResponse>)> OnAttempt)
		{
			return ClientRef->PostJsonRaw<FFlockTokenRevokeResponse>(Url, SessionRef->GetAuthHeaders(), TEXT("{}"), MoveTemp(OnAttempt));
		},
		[OnComplete](TFlockResult<FFlockTokenRevokeResponse> Result)
		{
			if (Result.bSuccess && !Result.Value.Revoked)
			{
				if (OnComplete)
				{
					OnComplete(TFlockResult<FFlockTokenRevokeResponse>::Fail(FFlockError::Make(EFlockErrorType::Auth,
						TEXT("Token revoke was not confirmed by the server"))));
				}
				return;
			}
			if (OnComplete)
			{
				OnComplete(Result);
			}
		},
		TEXT("Token revoke"), /*bIdempotent*/ false);
}

FFlockRequestHandle FFlockAuthProvider::IsNameAvailable(const FString& Name,
	TFunction<void(TFlockResult<FFlockNameAvailableResponse>)> OnComplete)
{
	if (!RequireNotEmpty<FFlockNameAvailableResponse>(Name, TEXT("Name"), OnComplete))
	{
		return FFlockRequestHandle();
	}
	const TSharedRef<FFlockHttpClient> ClientRef = Client;
	const TSharedRef<FFlockAuthSession> SessionRef = Session;
	const FString Url = AuthUrl(FlockEndpoints::PlayerNameAvailable(Name));
	return Execute<FFlockNameAvailableResponse>(
		[ClientRef, SessionRef, Url](TFunction<void(TFlockResult<FFlockNameAvailableResponse>)> OnAttempt)
		{
			return ClientRef->GetRaw<FFlockNameAvailableResponse>(Url, SessionRef->GetAuthHeaders(), MoveTemp(OnAttempt));
		},
		MoveTemp(OnComplete), TEXT("Name availability"));
}
