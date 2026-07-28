// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockCommandAsyncActions.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "FlockSubsystem.h"
#include "Providers/FlockCommandProvider.h"

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

	FFlockCommandProvider* ResolveCommands(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockCommandProvider* Provider = Sdk ? Sdk->GetCommandProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock command provider is not available. Initialize the SDK first."));
		}
		return Provider;
	}
}

// ───────────────────────────── Update Player Data ───────────────────────────

UFlockUpdatePlayerDataAction* UFlockUpdatePlayerDataAction::UpdatePlayerData(UObject* WorldContextObject,
	const FString& PlayerDataId, const FFlockCommandData& Data)
{
	UFlockUpdatePlayerDataAction* Action = NewObject<UFlockUpdatePlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerDataId = PlayerDataId;
	Action->Data = Data;
	return Action;
}

UFlockUpdatePlayerDataAction* UFlockUpdatePlayerDataAction::UpdatePlayerDataField(UObject* WorldContextObject,
	const FString& PlayerDataId, const FString& Key, const FFlockCommandValue& Value)
{
	UFlockUpdatePlayerDataAction* Action = NewObject<UFlockUpdatePlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerDataId = PlayerDataId;
	Action->Key = Key;
	Action->Value = Value;
	return Action;
}

void UFlockUpdatePlayerDataAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerData>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockUpdatePlayerDataAction> WeakThis(this);
	auto OnDone = [WeakThis](TFlockResult<FFlockPlayerData> Result)
	{
		if (UFlockUpdatePlayerDataAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	if (Key.IsEmpty())
	{
		Provider->UpdatePlayerData(PlayerDataId, Data, OnDone);
	}
	else
	{
		Provider->UpdatePlayerDataField(PlayerDataId, Key, Value, OnDone);
	}
}

void UFlockUpdatePlayerDataAction::Complete(const TFlockResult<FFlockPlayerData>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerData(), Result.Error);
	}
	SetReadyToDestroy();
}

// ───────────────────────────── Unlock Achievement ───────────────────────────

UFlockUnlockAchievementAction* UFlockUnlockAchievementAction::UnlockAchievement(UObject* WorldContextObject,
	const FString& AchievementName)
{
	UFlockUnlockAchievementAction* Action = NewObject<UFlockUnlockAchievementAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->AchievementName = AchievementName;
	return Action;
}

void UFlockUnlockAchievementAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerData>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockUnlockAchievementAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->UnlockAchievement(AchievementName, [WeakThis](TFlockResult<FFlockPlayerData> Result)
	{
		if (UFlockUnlockAchievementAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockUnlockAchievementAction::Complete(const TFlockResult<FFlockPlayerData>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerData(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────────── Add Game Funds ─────────────────────────────

UFlockAddGameFundsAction* UFlockAddGameFundsAction::AddGameFunds(UObject* WorldContextObject, const FString& Currency,
	int32 Amount, const FString& CurrencyTemplateId)
{
	UFlockAddGameFundsAction* Action = NewObject<UFlockAddGameFundsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Currency = Currency;
	Action->Amount = Amount;
	Action->CurrencyTemplateId = CurrencyTemplateId;
	return Action;
}

void UFlockAddGameFundsAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerData>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockAddGameFundsAction> WeakThis(this);
	auto OnDone = [WeakThis](TFlockResult<FFlockPlayerData> Result)
	{
		if (UFlockAddGameFundsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	if (CurrencyTemplateId.IsEmpty())
	{
		Provider->AddGameFunds(Currency, Amount, OnDone);
	}
	else
	{
		Provider->AddGameFunds(Currency, Amount, CurrencyTemplateId, OnDone);
	}
}

void UFlockAddGameFundsAction::Complete(const TFlockResult<FFlockPlayerData>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerData(), Result.Error);
	}
	SetReadyToDestroy();
}

// ───────────────────────── Flush Pending Commands ───────────────────────────

UFlockFlushPendingCommandsAction* UFlockFlushPendingCommandsAction::FlushPendingCommands(UObject* WorldContextObject)
{
	UFlockFlushPendingCommandsAction* Action = NewObject<UFlockFlushPendingCommandsAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockFlushPendingCommandsAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<int32>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockFlushPendingCommandsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->FlushPendingWrites([WeakThis](TFlockResult<int32> Result)
	{
		if (UFlockFlushPendingCommandsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockFlushPendingCommandsAction::Complete(const TFlockResult<int32>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(0, Result.Error);
	}
	SetReadyToDestroy();
}
