// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockAuthAsyncActions.h"
#include "FlockSubsystem.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Providers/FlockAuthProvider.h"

namespace
{
	/**
	 * Names the Blueprint that activated a node, for the SDK log's call-origin tag. The world context is
	 * the graph's `self` — a Blueprint-generated class for BP callers (its "_C" suffix is an artifact of
	 * the generated class, not part of the asset name), anything else means the node was driven from C++.
	 */
	FString ResolveCallOrigin(const UObject* WorldContextObject)
	{
		if (const UBlueprintGeneratedClass* BlueprintClass =
			WorldContextObject ? Cast<UBlueprintGeneratedClass>(WorldContextObject->GetClass()) : nullptr)
		{
			FString AssetName = BlueprintClass->GetName();
			AssetName.RemoveFromEnd(TEXT("_C"));
			return FString::Printf(TEXT("Blueprint '%s'"), *AssetName);
		}
		return TEXT("Blueprint node");
	}

	FFlockAuthProvider* ResolveProvider(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockAuthProvider* Provider = Sdk ? Sdk->GetAuthProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock SDK is not initialized. Initialize the SDK before calling auth nodes."));
		}
		return Provider;
	}
}

// ── UFlockLoginAction ──

UFlockLoginAction* UFlockLoginAction::Make(UObject* InWorldContextObject,
	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)>)> InStart)
{
	UFlockLoginAction* Action = NewObject<UFlockLoginAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->Start = MoveTemp(InStart);
	Action->RegisterWithGameInstance(InWorldContextObject);
	return Action;
}

UFlockLoginAction* UFlockLoginAction::LoginWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password)
{
	return Make(WorldContextObject,
		[Email, Password](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithEmail(Email, Password, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithDevice(UObject* WorldContextObject, const FString& DeviceId)
{
	return Make(WorldContextObject,
		[DeviceId](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithDevice(DeviceId, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithGoogle(UObject* WorldContextObject, const FString& IdToken)
{
	return Make(WorldContextObject,
		[IdToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithGoogle(IdToken, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithApple(UObject* WorldContextObject, const FString& IdentityToken)
{
	return Make(WorldContextObject,
		[IdentityToken](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithApple(IdentityToken, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithSteam(UObject* WorldContextObject, const FString& SessionTicket)
{
	return Make(WorldContextObject,
		[SessionTicket](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithSteam(SessionTicket, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithFacebook(UObject* WorldContextObject, const FString& FacebookId)
{
	return Make(WorldContextObject,
		[FacebookId](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithFacebook(FacebookId, MoveTemp(OnComplete));
		});
}

UFlockLoginAction* UFlockLoginAction::LoginWithDiscord(UObject* WorldContextObject, const FString& DiscordId)
{
	return Make(WorldContextObject,
		[DiscordId](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockPlayerLoginResponse>)> OnComplete)
		{
			return Provider.LoginWithDiscord(DiscordId, MoveTemp(OnComplete));
		});
}

void UFlockLoginAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerLoginResponse>::Fail(Error));
		return;
	}
	TWeakObjectPtr<UFlockLoginAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
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

UFlockRegisterAction* UFlockRegisterAction::Make(UObject* InWorldContextObject,
	TFunction<FFlockRequestHandle(FFlockAuthProvider&, TFunction<void(TFlockResult<FFlockRegisterResult>)>)> InStart)
{
	UFlockRegisterAction* Action = NewObject<UFlockRegisterAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->Start = MoveTemp(InStart);
	Action->RegisterWithGameInstance(InWorldContextObject);
	return Action;
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithEmail(UObject* WorldContextObject, const FString& Email, const FString& Password, const FString& Name)
{
	return Make(WorldContextObject,
		[Email, Password, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithEmail(Email, Password, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithDevice(UObject* WorldContextObject, const FString& DeviceId, const FString& Name)
{
	return Make(WorldContextObject,
		[DeviceId, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithDevice(DeviceId, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithGoogle(UObject* WorldContextObject, const FString& IdToken, const FString& Name)
{
	return Make(WorldContextObject,
		[IdToken, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithGoogle(IdToken, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithApple(UObject* WorldContextObject, const FString& IdentityToken, const FString& Name)
{
	return Make(WorldContextObject,
		[IdentityToken, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithApple(IdentityToken, Name, MoveTemp(OnComplete));
		});
}

UFlockRegisterAction* UFlockRegisterAction::RegisterWithSteam(UObject* WorldContextObject, const FString& SessionTicket, const FString& Name)
{
	return Make(WorldContextObject,
		[SessionTicket, Name](FFlockAuthProvider& Provider, TFunction<void(TFlockResult<FFlockRegisterResult>)> OnComplete)
		{
			return Provider.RegisterWithSteam(SessionTicket, Name, MoveTemp(OnComplete));
		});
}

void UFlockRegisterAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockRegisterResult>::Fail(Error));
		return;
	}
	TWeakObjectPtr<UFlockRegisterAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
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

UFlockRestoreSessionAction* UFlockRestoreSessionAction::RestoreSession(UObject* WorldContextObject)
{
	UFlockRestoreSessionAction* Action = NewObject<UFlockRestoreSessionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UFlockRestoreSessionAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject, Error);
	if (!Provider)
	{
		OnNoSession.Broadcast();
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UFlockRestoreSessionAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
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

UFlockAuthAccountAction* UFlockAuthAccountAction::Make(UObject* InWorldContextObject,
	TFunction<void(FFlockAuthProvider&, TFunction<void(bool, const FFlockError&)>)> InStart)
{
	UFlockAuthAccountAction* Action = NewObject<UFlockAuthAccountAction>();
	Action->WorldContextObject = InWorldContextObject;
	Action->Start = MoveTemp(InStart);
	Action->RegisterWithGameInstance(InWorldContextObject);
	return Action;
}

UFlockAuthAccountAction* UFlockAuthAccountAction::ForgotPassword(UObject* WorldContextObject, const FString& Email)
{
	return Make(WorldContextObject,
		[Email](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			// The backend always reports success here; surface its flag so a false is visible.
			Provider.ForgotPassword(Email, [OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess && R.Value.Success, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::ResetPassword(UObject* WorldContextObject, const FString& Email, const FString& Code, const FString& NewPassword)
{
	return Make(WorldContextObject,
		[Email, Code, NewPassword](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.ResetPassword(Email, Code, NewPassword, [OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::SendEmailVerification(UObject* WorldContextObject)
{
	return Make(WorldContextObject,
		[](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.SendEmailVerification([OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::VerifyEmail(UObject* WorldContextObject, const FString& Code)
{
	return Make(WorldContextObject,
		[Code](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.VerifyEmail(Code, [OnDone](TFlockResult<FFlockAuthActionResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::RevokeToken(UObject* WorldContextObject)
{
	return Make(WorldContextObject,
		[](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.RevokeToken([OnDone](TFlockResult<FFlockTokenRevokeResponse> R)
			{
				OnDone(R.bSuccess, R.Error);
			});
		});
}

UFlockAuthAccountAction* UFlockAuthAccountAction::RefreshToken(UObject* WorldContextObject)
{
	return Make(WorldContextObject,
		[](FFlockAuthProvider& Provider, TFunction<void(bool, const FFlockError&)> OnDone)
		{
			Provider.RefreshToken([OnDone](bool bRefreshed)
			{
				OnDone(bRefreshed, bRefreshed
					? FFlockError()
					: FFlockError::Make(EFlockErrorType::Auth, TEXT("Token refresh failed. Please log in again.")));
			});
		});
}

void UFlockAuthAccountAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(false, Error);
		return;
	}
	TWeakObjectPtr<UFlockAuthAccountAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
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

// ── UFlockNameAvailableAction ──

UFlockNameAvailableAction* UFlockNameAvailableAction::IsNameAvailable(UObject* WorldContextObject, const FString& Name)
{
	UFlockNameAvailableAction* Action = NewObject<UFlockNameAvailableAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Name = Name;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UFlockNameAvailableAction::Activate()
{
	FFlockError Error;
	FFlockAuthProvider* Provider = ResolveProvider(WorldContextObject, Error);
	if (!Provider)
	{
		OnFailure.Broadcast(false, Error);
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UFlockNameAvailableAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
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
