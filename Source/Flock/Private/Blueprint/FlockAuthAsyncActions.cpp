// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockAuthAsyncActions.h"
#include "Blueprint/FlockBlueprintCallOrigin.h"
#include "Http/FlockErrorHints.h"
#include "FlockLocalPlayer.h"
#include "FlockSubsystem.h"
#include "Providers/FlockAuthProvider.h"

namespace
{
	FFlockAuthProvider* ResolveProvider(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer, FFlockError& OutError)
	{
		if (LocalPlayer)
		{
			return LocalPlayer->GetAuthProvider();
		}
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockAuthProvider* Provider = Sdk ? Sdk->GetAuthProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("Flock is not initialized, so its sign-in nodes cannot run"), FFlockErrorHints::InitializeFlockFirst());
		}
		return Provider;
	}
}

// ── UFlockLoginAction ──

UFlockLoginAction* UFlockLoginAction::Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)>)> InStart)
{
	UFlockLoginAction* Action = NewObject<UFlockLoginAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->LocalPlayer = InLocalPlayer;
	Action->Start = MoveTemp(InStart);
	Action->RegisterWithGameInstance(InWorldContextObject);
	return Action;
}

UFlockLoginAction* UFlockLoginAction::LoginWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[Email, Password](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithEmail(Email, Password, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithDevice(UObject* WorldContextObject, const FString& DeviceId, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[DeviceId](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithDevice(DeviceId, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithGoogle(UObject* WorldContextObject, const FString& IdToken, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[IdToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithGoogle(IdToken, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithApple(UObject* WorldContextObject, const FString& IdentityToken, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[IdentityToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithApple(IdentityToken, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithSteam(UObject* WorldContextObject, const FString& SessionTicket, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[SessionTicket](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithSteam(SessionTicket, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithFacebook(UObject* WorldContextObject, const FString& FacebookId, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[FacebookId](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithFacebook(FacebookId, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithDiscord(UObject* WorldContextObject, const FString& DiscordId, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[DiscordId](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithDiscord(DiscordId, MoveTemp(OnComplete));
		});
}

void UFlockLoginAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject.Get(), LocalPlayer, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerLoginResponse>::Fail(Error));
		return;
	}
	TWeakObjectPtr<UFlockLoginAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Start(*Provider, [WeakThis](TFlockResult<FFlockPlayerLoginResponse> Result)
	{
		if (UFlockLoginAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockLoginAction::Complete(const TFlockResult<FFlockPlayerLoginResponse>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, Result.Error);
	}
	else
	{
		OnFailure.Broadcast(Result.Value, Result.Error);
	}
	SetReadyToDestroy();
}

// ── UFlockRegisterAction ──

UFlockRegisterAction* UFlockRegisterAction::Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockRegisterResult>)>)> InStart)
{
	UFlockRegisterAction* Action = NewObject<UFlockRegisterAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->LocalPlayer = InLocalPlayer;
	Action->Start = MoveTemp(InStart);
	Action->RegisterWithGameInstance(InWorldContextObject);
	return Action;
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, const FString& Name, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[Email, Password, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithEmail(Email, Password, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithDevice(UObject* WorldContextObject, const FString& DeviceId, const FString& Name, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[DeviceId, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithDevice(DeviceId, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithGoogle(UObject* WorldContextObject, const FString& IdToken, const FString& Name, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[IdToken, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithGoogle(IdToken, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithApple(UObject* WorldContextObject, const FString& IdentityToken, const FString& Name, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[IdentityToken, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithApple(IdentityToken, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithSteam(UObject* WorldContextObject, const FString& SessionTicket, const FString& Name, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[SessionTicket, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithSteam(SessionTicket, Name, MoveTemp(OnComplete));
		});
}

void UFlockRegisterAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject.Get(), LocalPlayer, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockRegisterResult>::Fail(Error));
		return;
	}
	TWeakObjectPtr<UFlockRegisterAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Start(*Provider, [WeakThis](TFlockResult<FFlockRegisterResult> Result)
	{
		if (UFlockRegisterAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockRegisterAction::Complete(const TFlockResult<FFlockRegisterResult>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, Result.Error);
	}
	else
	{
		OnFailure.Broadcast(Result.Value, Result.Error);
	}
	SetReadyToDestroy();
}

// ── UFlockRestoreSessionAction ──

UFlockRestoreSessionAction* UFlockRestoreSessionAction::RestoreSession(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer)
{
	UFlockRestoreSessionAction* Action = NewObject<UFlockRestoreSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UFlockRestoreSessionAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject.Get(), LocalPlayer, Error);
	if (!Provider)
	{
		OnNoSession.Broadcast();
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UFlockRestoreSessionAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->TryRestoreSession([WeakThis](bool bRestored)
	{
		if (UFlockRestoreSessionAction* Self = WeakThis.Get())
		{
			if (bRestored)
			{
				Self->OnRestored.Broadcast();
			}
			else
			{
				Self->OnNoSession.Broadcast();
			}
			Self->SetReadyToDestroy();
		}
	});
}

// ── UFlockAuthAccountAction ──

UFlockAuthAccountAction* UFlockAuthAccountAction::Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
	TFunction<void(FFlockAuthProvider&, TFunction<void(bool, const FFlockError&)>)> InStart)
{
	UFlockAuthAccountAction* Action = NewObject<UFlockAuthAccountAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->LocalPlayer = InLocalPlayer;
	Action->Start = MoveTemp(InStart);
	Action->RegisterWithGameInstance(InWorldContextObject);
	return Action;
}

UFlockAuthAccountAction* UFlockAuthAccountAction::ForgotPassword(UObject* WorldContextObject, const FString& Email, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[Email](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			// The backend always reports success here; surface its flag so a false is visible.
			Provider.ForgotPassword(Email, [OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess && R.Value.Success, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::ResetPassword(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& NewPassword, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[Email, Code, NewPassword](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.ResetPassword(Email, Code, NewPassword, [OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::SendEmailVerification(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.SendEmailVerification([OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::VerifyEmail(UObject* WorldContextObject, const FString& Code, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[Code](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.VerifyEmail(Code, [OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::RevokeToken(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.RevokeToken([OnDone](TFlockResult<FFlockTokenRevokeResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::RefreshToken(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.RefreshToken([OnDone](bool bRefreshed)
			{
				OnDone(bRefreshed, bRefreshed
					? FFlockError()
					: FFlockError::MakeWithFix(EFlockErrorType::Auth, TEXT("The player's sign-in could not be renewed"), TEXT("Sign the player in again. If the device was offline, wait for the connection and renew again first; the Output Log names what refused it.")));
			});
		});
}

void UFlockAuthAccountAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject.Get(), LocalPlayer, Error);
	if (!Provider)
	{
		Complete(false, Error);
		return;
	}
	TWeakObjectPtr<UFlockAuthAccountAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Start(*Provider, [WeakThis](bool bSuccess, const FFlockError& ResultError)
	{
		if (UFlockAuthAccountAction* Self = WeakThis.Get())
		{
			Self->Complete(bSuccess, ResultError);
		}
	});
}

void UFlockAuthAccountAction::Complete(bool bSuccess, const FFlockError& Error)
{
	if (bSuccess)
	{
		OnSuccess.Broadcast(Error);
	}
	else
	{
		OnFailure.Broadcast(Error);
	}
	SetReadyToDestroy();
}

// ── UFlockAccountLinkAction ──

UFlockAccountLinkAction* UFlockAccountLinkAction::Make(UObject* InWorldContextObject, UFlockLocalPlayer* InLocalPlayer,
	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)>)> InStart)
{
	UFlockAccountLinkAction* Action = NewObject<UFlockAccountLinkAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->LocalPlayer = InLocalPlayer;
	Action->Start = MoveTemp(InStart);
	Action->RegisterWithGameInstance(InWorldContextObject);
	return Action;
}

UFlockAccountLinkAction* UFlockAccountLinkAction::GetLinkedAccounts(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.GetLinkedAccounts(MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::LinkEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[Email, Password](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.LinkEmail(Email, Password, MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::LinkDevice(UObject* WorldContextObject, const FString& DeviceId, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[DeviceId](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.LinkDevice(DeviceId, MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::LinkGoogle(UObject* WorldContextObject, const FString& IdToken, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[IdToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.LinkGoogle(IdToken, MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::LinkApple(UObject* WorldContextObject, const FString& IdentityToken, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[IdentityToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.LinkApple(IdentityToken, MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::LinkSteam(UObject* WorldContextObject, const FString& SessionTicket, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[SessionTicket](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.LinkSteam(SessionTicket, MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::LinkFacebook(UObject* WorldContextObject, const FString& FacebookToken, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[FacebookToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.LinkFacebook(FacebookToken, MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::LinkDiscord(UObject* WorldContextObject, const FString& DiscordToken, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[DiscordToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return Provider.LinkDiscord(DiscordToken, MoveTemp(OnDone));
		});
}

UFlockAccountLinkAction* UFlockAccountLinkAction::Unlink(UObject* WorldContextObject, EFlockCredentialProvider Provider, UFlockLocalPlayer* LocalPlayer)
{
	return Make(WorldContextObject, LocalPlayer,
		[Provider](FFlockAuthProvider& AuthProvider, TFunction<void(TFlockResult<FFlockPlayerAccountsResponse>)> OnDone)
		{
			return AuthProvider.Unlink(Provider, MoveTemp(OnDone));
		});
}

void UFlockAccountLinkAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject.Get(), LocalPlayer, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerAccountsResponse>::Fail(Error));
		return;
	}
	TWeakObjectPtr<UFlockAccountLinkAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Start(*Provider, [WeakThis](TFlockResult<FFlockPlayerAccountsResponse> Result)
	{
		if (UFlockAccountLinkAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockAccountLinkAction::Complete(const TFlockResult<FFlockPlayerAccountsResponse>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value.Accounts, Result.Error);
	}
	else
	{
		// The unused half of the payload is default-constructed, per the node convention.
		OnFailure.Broadcast(TArray<FFlockPlayerLinkedAccount>(), Result.Error);
	}
	SetReadyToDestroy();
}

// ── UFlockNameAvailableAction ──

UFlockNameAvailableAction* UFlockNameAvailableAction::IsNameAvailable(UObject* WorldContextObject, const FString& Name, UFlockLocalPlayer* LocalPlayer)
{
	UFlockNameAvailableAction* Action = NewObject<UFlockNameAvailableAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UFlockNameAvailableAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject.Get(), LocalPlayer, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(false, Error);
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UFlockNameAvailableAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->IsNameAvailable(Name, [WeakThis](TFlockResult<FFlockNameAvailableResponse> Result)
	{
		if (UFlockNameAvailableAction* Self = WeakThis.Get())
		{
			if (Result.bSuccess)
			{
				Self->OnResult.Broadcast(Result.Value.Available, Result.Error);
			}
			else
			{
				Self->OnFailure.Broadcast(false, Result.Error);
			}
			Self->SetReadyToDestroy();
		}
	});
}

// ── UFlockRemoveLocalPlayerAction ──

UFlockRemoveLocalPlayerAction* UFlockRemoveLocalPlayerAction::RemoveLocalPlayer(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer)
{
	UFlockRemoveLocalPlayerAction* Action = NewObject<UFlockRemoveLocalPlayerAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UFlockRemoveLocalPlayerAction::Activate()
{
	UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject.Get());
	if (!Sdk || !Sdk->IsInitialized())
	{
		Complete(TFlockResult<FFlockTokenRevokeResponse>::Fail(FFlockError::MakeWithFix(EFlockErrorType::Validation,
			TEXT("Flock is not initialized, so a local player cannot be removed"), FFlockErrorHints::InitializeFlockFirst())));
		return;
	}
	TWeakObjectPtr<UFlockRemoveLocalPlayerAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Sdk->RemoveLocalPlayer(LocalPlayer, [WeakThis](TFlockResult<FFlockTokenRevokeResponse> Result)
	{
		if (UFlockRemoveLocalPlayerAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockRemoveLocalPlayerAction::Complete(const TFlockResult<FFlockTokenRevokeResponse>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, Result.Error);
	}
	else
	{
		OnFailure.Broadcast(Result.Value, Result.Error);
	}
	SetReadyToDestroy();
}
