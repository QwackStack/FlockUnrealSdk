// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockCommandAsyncActions.h"
#include "Blueprint/FlockBlueprintCallOrigin.h"
#include "Http/FlockErrorHints.h"

#include "FlockLocalPlayer.h"
#include "FlockSubsystem.h"
#include "Providers/FlockCommandProvider.h"

namespace
{
	FFlockCommandProvider* ResolveCommands(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer, FFlockError& OutError)
	{
		if (LocalPlayer)
		{
			return LocalPlayer->GetCommandProvider();
		}
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockCommandProvider* Provider = Sdk ? Sdk->GetCommandProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("Flock commands are not available: Flock is not initialized"), FFlockErrorHints::InitializeFlockFirst());
		}
		return Provider;
	}
}

// ───────────────────────────── Update Player Data ───────────────────────────

UFlockUpdatePlayerDataAction* UFlockUpdatePlayerDataAction::UpdatePlayerData(UObject* WorldContextObject,
	const FString& PlayerDataId, const FFlockCommandData& Data, UFlockLocalPlayer* LocalPlayer)
{
	UFlockUpdatePlayerDataAction* Action = NewObject<UFlockUpdatePlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	Action->PlayerDataId = PlayerDataId;
	Action->Data = Data;
	return Action;
}

UFlockUpdatePlayerDataAction* UFlockUpdatePlayerDataAction::UpdatePlayerDataField(UObject* WorldContextObject,
	const FString& PlayerDataId, const FString& Key, const FFlockCommandValue& Value, UFlockLocalPlayer* LocalPlayer)
{
	UFlockUpdatePlayerDataAction* Action = NewObject<UFlockUpdatePlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	Action->PlayerDataId = PlayerDataId;
	Action->Key = Key;
	Action->Value = Value;
	return Action;
}

void UFlockUpdatePlayerDataAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, LocalPlayer, Error);
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

	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
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
	const FString& AchievementName, UFlockLocalPlayer* LocalPlayer)
{
	UFlockUnlockAchievementAction* Action = NewObject<UFlockUnlockAchievementAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	Action->AchievementName = AchievementName;
	return Action;
}

void UFlockUnlockAchievementAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, LocalPlayer, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerData>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockUnlockAchievementAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
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
	int32 Amount, const FString& CurrencyTemplateId, UFlockLocalPlayer* LocalPlayer)
{
	UFlockAddGameFundsAction* Action = NewObject<UFlockAddGameFundsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	Action->Currency = Currency;
	Action->Amount = Amount;
	Action->CurrencyTemplateId = CurrencyTemplateId;
	return Action;
}

void UFlockAddGameFundsAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, LocalPlayer, Error);
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

	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
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

UFlockFlushPendingCommandsAction* UFlockFlushPendingCommandsAction::FlushPendingCommands(UObject* WorldContextObject, UFlockLocalPlayer* LocalPlayer)
{
	UFlockFlushPendingCommandsAction* Action = NewObject<UFlockFlushPendingCommandsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->LocalPlayer = LocalPlayer;
	return Action;
}

void UFlockFlushPendingCommandsAction::Activate()
{
	FFlockError Error;
	FFlockCommandProvider* Provider = ResolveCommands(WorldContextObject, LocalPlayer, Error);
	if (!Provider)
	{
		Complete(TFlockResult<int32>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockFlushPendingCommandsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
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
