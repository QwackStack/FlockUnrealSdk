// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockPlayerAsyncActions.h"
#include "Blueprint/FlockBlueprintCallOrigin.h"
#include "Http/FlockErrorHints.h"

#include "FlockSubsystem.h"
#include "Providers/FlockPlayerProvider.h"

namespace
{
	FFlockPlayerProvider* ResolvePlayer(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockPlayerProvider* Provider = Sdk ? Sdk->GetPlayerProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::MakeWithFix(EFlockErrorType::Validation,
				TEXT("Flock player data is not available: Flock is not initialized"), FFlockErrorHints::InitializeFlockFirst());
		}
		return Provider;
	}
}

// ─────────────────────────── Get Player Data By Id ──────────────────────────

UFlockGetPlayerDataAction* UFlockGetPlayerDataAction::GetPlayerDataById(UObject* WorldContextObject, const FString& PlayerDataId)
{
	UFlockGetPlayerDataAction* Action = NewObject<UFlockGetPlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerDataId = PlayerDataId;
	return Action;
}

void UFlockGetPlayerDataAction::Activate()
{
	FFlockError Error;
	FFlockPlayerProvider* Provider = ResolvePlayer(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerData>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetPlayerDataAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	Provider->GetDataById(PlayerDataId, [WeakThis](TFlockResult<FFlockPlayerData> Result)
	{
		if (UFlockGetPlayerDataAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetPlayerDataAction::Complete(const TFlockResult<FFlockPlayerData>& Result)
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

// ─────────────────────────── Get All Player Data ────────────────────────────

UFlockGetAllPlayerDataAction* UFlockGetAllPlayerDataAction::GetAllPlayerData(UObject* WorldContextObject,
	const FString& PlayerId, int32 Page, int32 Limit)
{
	UFlockGetAllPlayerDataAction* Action = NewObject<UFlockGetAllPlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerId = PlayerId;
	Action->Page = Page;
	Action->Limit = Limit;
	return Action;
}

void UFlockGetAllPlayerDataAction::Activate()
{
	FFlockError Error;
	FFlockPlayerProvider* Provider = ResolvePlayer(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerDataPage>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetAllPlayerDataAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	Provider->GetAllData(PlayerId, Page, Limit, [WeakThis](TFlockResult<FFlockPlayerDataPage> Result)
	{
		if (UFlockGetAllPlayerDataAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetAllPlayerDataAction::Complete(const TFlockResult<FFlockPlayerDataPage>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerDataPage(), Result.Error);
	}
	SetReadyToDestroy();
}

// ──────────────────────────── Get My Player Data ────────────────────────────

UFlockGetMyPlayerDataAction* UFlockGetMyPlayerDataAction::GetMyDataByTemplate(UObject* WorldContextObject, const FString& PlayerTemplateId)
{
	UFlockGetMyPlayerDataAction* Action = NewObject<UFlockGetMyPlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByTag = false;
	Action->TemplateIdOrTag = PlayerTemplateId;
	return Action;
}

UFlockGetMyPlayerDataAction* UFlockGetMyPlayerDataAction::GetMyDataByTag(UObject* WorldContextObject, const FString& Tag)
{
	UFlockGetMyPlayerDataAction* Action = NewObject<UFlockGetMyPlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByTag = true;
	Action->TemplateIdOrTag = Tag;
	return Action;
}

void UFlockGetMyPlayerDataAction::Activate()
{
	FFlockError Error;
	FFlockPlayerProvider* Provider = ResolvePlayer(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerData>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetMyPlayerDataAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	auto OnDone = [WeakThis](TFlockResult<FFlockPlayerData> Result)
	{
		if (UFlockGetMyPlayerDataAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	if (bByTag)
	{
		Provider->GetMyDataByTag(TemplateIdOrTag, OnDone);
	}
	else
	{
		Provider->GetMyDataByTemplate(TemplateIdOrTag, OnDone);
	}
}

void UFlockGetMyPlayerDataAction::Complete(const TFlockResult<FFlockPlayerData>& Result)
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

// ─────────────────────────── Get Player Templates ───────────────────────────

UFlockGetPlayerTemplatesAction* UFlockGetPlayerTemplatesAction::GetPlayerTemplates(UObject* WorldContextObject)
{
	UFlockGetPlayerTemplatesAction* Action = NewObject<UFlockGetPlayerTemplatesAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetPlayerTemplatesAction::Activate()
{
	FFlockError Error;
	FFlockPlayerProvider* Provider = ResolvePlayer(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<TArray<FFlockPlayerTemplateSchema>>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetPlayerTemplatesAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	Provider->GetTemplates([WeakThis](TFlockResult<TArray<FFlockPlayerTemplateSchema>> Result)
	{
		if (UFlockGetPlayerTemplatesAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetPlayerTemplatesAction::Complete(const TFlockResult<TArray<FFlockPlayerTemplateSchema>>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(TArray<FFlockPlayerTemplateSchema>(), Result.Error);
	}
	SetReadyToDestroy();
}

// ──────────────────────────── Get Player Template ───────────────────────────

UFlockGetPlayerTemplateAction* UFlockGetPlayerTemplateAction::GetPlayerTemplateById(UObject* WorldContextObject, const FString& PlayerTemplateId)
{
	UFlockGetPlayerTemplateAction* Action = NewObject<UFlockGetPlayerTemplateAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Mode = ELookup::ById;
	Action->TemplateIdNameOrTag = PlayerTemplateId;
	return Action;
}

UFlockGetPlayerTemplateAction* UFlockGetPlayerTemplateAction::GetPlayerTemplateByName(UObject* WorldContextObject, const FString& Name)
{
	UFlockGetPlayerTemplateAction* Action = NewObject<UFlockGetPlayerTemplateAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Mode = ELookup::ByName;
	Action->TemplateIdNameOrTag = Name;
	return Action;
}

UFlockGetPlayerTemplateAction* UFlockGetPlayerTemplateAction::GetPlayerTemplateByTag(UObject* WorldContextObject, const FString& Tag)
{
	UFlockGetPlayerTemplateAction* Action = NewObject<UFlockGetPlayerTemplateAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Mode = ELookup::ByTag;
	Action->TemplateIdNameOrTag = Tag;
	return Action;
}

void UFlockGetPlayerTemplateAction::Activate()
{
	FFlockError Error;
	FFlockPlayerProvider* Provider = ResolvePlayer(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerTemplateSchema>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetPlayerTemplateAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	auto OnDone = [WeakThis](TFlockResult<FFlockPlayerTemplateSchema> Result)
	{
		if (UFlockGetPlayerTemplateAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	switch (Mode)
	{
	case ELookup::ByName:
		Provider->GetTemplateByName(TemplateIdNameOrTag, OnDone);
		break;
	case ELookup::ByTag:
		Provider->GetTemplateByTag(TemplateIdNameOrTag, OnDone);
		break;
	case ELookup::ById:
	default:
		Provider->GetTemplateById(TemplateIdNameOrTag, OnDone);
		break;
	}
}

void UFlockGetPlayerTemplateAction::Complete(const TFlockResult<FFlockPlayerTemplateSchema>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerTemplateSchema(), Result.Error);
	}
	SetReadyToDestroy();
}

// ───────────────────────── Get Template Player Data ─────────────────────────

UFlockGetTemplatePlayerDataAction* UFlockGetTemplatePlayerDataAction::GetTemplatePlayerData(UObject* WorldContextObject, const FString& PlayerTemplateId)
{
	UFlockGetTemplatePlayerDataAction* Action = NewObject<UFlockGetTemplatePlayerDataAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerTemplateId = PlayerTemplateId;
	return Action;
}

void UFlockGetTemplatePlayerDataAction::Activate()
{
	FFlockError Error;
	FFlockPlayerProvider* Provider = ResolvePlayer(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<TArray<FFlockPlayerData>>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetTemplatePlayerDataAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	Provider->GetTemplatePlayerData(PlayerTemplateId, [WeakThis](TFlockResult<TArray<FFlockPlayerData>> Result)
	{
		if (UFlockGetTemplatePlayerDataAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetTemplatePlayerDataAction::Complete(const TFlockResult<TArray<FFlockPlayerData>>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(TArray<FFlockPlayerData>(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────────── Get Player Ban ─────────────────────────────

UFlockGetPlayerBanAction* UFlockGetPlayerBanAction::GetPlayerBan(UObject* WorldContextObject, const FString& PlayerId)
{
	UFlockGetPlayerBanAction* Action = NewObject<UFlockGetPlayerBanAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerId = PlayerId;
	return Action;
}

void UFlockGetPlayerBanAction::Activate()
{
	FFlockError Error;
	FFlockPlayerProvider* Provider = ResolvePlayer(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerBan>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetPlayerBanAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject));
	Provider->GetBan(PlayerId, [WeakThis](TFlockResult<FFlockPlayerBan> Result)
	{
		if (UFlockGetPlayerBanAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetPlayerBanAction::Complete(const TFlockResult<FFlockPlayerBan>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerBan(), Result.Error);
	}
	SetReadyToDestroy();
}
