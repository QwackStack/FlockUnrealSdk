// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockConfigAsyncActions.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "FlockSubsystem.h"
#include "Providers/FlockConfigProvider.h"
#include "Providers/FlockGameProvider.h"

namespace
{
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

	FFlockConfigProvider* ResolveConfig(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockConfigProvider* Provider = Sdk ? Sdk->GetConfigProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock config is not available. Initialize the SDK first."));
		}
		return Provider;
	}

	FFlockGameProvider* ResolveGame(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockGameProvider* Provider = Sdk ? Sdk->GetGameProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock game info is not available. Initialize the SDK first."));
		}
		return Provider;
	}
}

// ─────────────────────────────── Get Config ────────────────────────────────

UFlockGetConfigAction* UFlockGetConfigAction::GetConfigByName(UObject* WorldContextObject, const FString& Name)
{
	UFlockGetConfigAction* Action = NewObject<UFlockGetConfigAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByName = true;
	Action->Argument = Name;
	return Action;
}

UFlockGetConfigAction* UFlockGetConfigAction::GetConfigById(UObject* WorldContextObject, const FString& ConfigId)
{
	UFlockGetConfigAction* Action = NewObject<UFlockGetConfigAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByName = false;
	Action->Argument = ConfigId;
	return Action;
}

void UFlockGetConfigAction::Activate()
{
	FFlockError Error;
	FFlockConfigProvider* Provider = ResolveConfig(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockGameConfigSchema>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetConfigAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	auto OnDone = [WeakThis](TFlockResult<FFlockGameConfigSchema> Result)
	{
		if (UFlockGetConfigAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	if (bByName)
	{
		Provider->GetConfigByName(Argument, OnDone);
	}
	else
	{
		Provider->GetConfigById(Argument, OnDone);
	}
}

void UFlockGetConfigAction::Complete(const TFlockResult<FFlockGameConfigSchema>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockGameConfigSchema(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────── Get Configs By Tag ────────────────────────────

UFlockGetConfigsByTagAction* UFlockGetConfigsByTagAction::GetConfigsByTag(UObject* WorldContextObject, EFlockConfigTag Tag)
{
	UFlockGetConfigsByTagAction* Action = NewObject<UFlockGetConfigsByTagAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Tag = Tag;
	return Action;
}

void UFlockGetConfigsByTagAction::Activate()
{
	FFlockError Error;
	FFlockConfigProvider* Provider = ResolveConfig(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<TArray<FFlockGameConfigSchema>>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetConfigsByTagAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetConfigsByTag(Tag, [WeakThis](TFlockResult<TArray<FFlockGameConfigSchema>> Result)
	{
		if (UFlockGetConfigsByTagAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetConfigsByTagAction::Complete(const TFlockResult<TArray<FFlockGameConfigSchema>>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(TArray<FFlockGameConfigSchema>(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────── Resolve Config Data ───────────────────────────

UFlockResolveConfigDataAction* UFlockResolveConfigDataAction::ResolveConfigData(UObject* WorldContextObject, const FString& ConfigId)
{
	UFlockResolveConfigDataAction* Action = NewObject<UFlockResolveConfigDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->ConfigId = ConfigId;
	return Action;
}

void UFlockResolveConfigDataAction::Activate()
{
	FFlockError Error;
	FFlockConfigProvider* Provider = ResolveConfig(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockStructuredData>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockResolveConfigDataAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->ResolveConfigData(ConfigId, [WeakThis](TFlockResult<FFlockStructuredData> Result)
	{
		if (UFlockResolveConfigDataAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockResolveConfigDataAction::Complete(const TFlockResult<FFlockStructuredData>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockStructuredData(), Result.Error);
	}
	SetReadyToDestroy();
}

// ──────────────────────────────── Get Game ─────────────────────────────────

UFlockGetGameAction* UFlockGetGameAction::GetGame(UObject* WorldContextObject)
{
	UFlockGetGameAction* Action = NewObject<UFlockGetGameAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetGameAction::Activate()
{
	FFlockError Error;
	FFlockGameProvider* Provider = ResolveGame(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockGameSchema>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetGameAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetGame([WeakThis](TFlockResult<FFlockGameSchema> Result)
	{
		if (UFlockGetGameAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetGameAction::Complete(const TFlockResult<FFlockGameSchema>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockGameSchema(), Result.Error);
	}
	SetReadyToDestroy();
}

// ──────────────────────────── Get Game Version ─────────────────────────────

UFlockGetGameVersionAction* UFlockGetGameVersionAction::GetGameVersion(UObject* WorldContextObject)
{
	UFlockGetGameVersionAction* Action = NewObject<UFlockGetGameVersionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByName = false;
	return Action;
}

UFlockGetGameVersionAction* UFlockGetGameVersionAction::GetGameVersionByName(UObject* WorldContextObject, const FString& Name)
{
	UFlockGetGameVersionAction* Action = NewObject<UFlockGetGameVersionAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByName = true;
	Action->Name = Name;
	return Action;
}

void UFlockGetGameVersionAction::Activate()
{
	FFlockError Error;
	FFlockGameProvider* Provider = ResolveGame(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockGameVersionSchema>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetGameVersionAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	auto OnDone = [WeakThis](TFlockResult<FFlockGameVersionSchema> Result)
	{
		if (UFlockGetGameVersionAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	if (bByName)
	{
		Provider->GetGameVersionByName(Name, OnDone);
	}
	else
	{
		Provider->GetGameVersion(OnDone);
	}
}

void UFlockGetGameVersionAction::Complete(const TFlockResult<FFlockGameVersionSchema>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockGameVersionSchema(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────── Get Player Features ───────────────────────────

UFlockGetPlayerFeaturesAction* UFlockGetPlayerFeaturesAction::GetPlayerFeatures(UObject* WorldContextObject, const FString& PlayerId)
{
	UFlockGetPlayerFeaturesAction* Action = NewObject<UFlockGetPlayerFeaturesAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerId = PlayerId;
	return Action;
}

void UFlockGetPlayerFeaturesAction::Activate()
{
	FFlockError Error;
	FFlockConfigProvider* Provider = ResolveConfig(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockGameConfigSchema>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetPlayerFeaturesAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetPlayerFeatures(PlayerId, [WeakThis](TFlockResult<FFlockGameConfigSchema> Result)
	{
		if (UFlockGetPlayerFeaturesAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetPlayerFeaturesAction::Complete(const TFlockResult<FFlockGameConfigSchema>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockGameConfigSchema(), Result.Error);
	}
	SetReadyToDestroy();
}
