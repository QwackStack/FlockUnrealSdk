// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockShopAsyncActions.h"

#include "Engine/BlueprintGeneratedClass.h"
#include "FlockSubsystem.h"
#include "Providers/FlockShopProvider.h"

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

	FFlockShopProvider* ResolveShop(UObject* WorldContextObject, FFlockError& OutError)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		FFlockShopProvider* Provider = Sdk ? Sdk->GetShopProvider() : nullptr;
		if (!Provider)
		{
			OutError = FFlockError::Make(EFlockErrorType::Validation,
				TEXT("Flock shop is not available. Initialize the SDK first."));
		}
		return Provider;
	}
}

// ────────────────────────────── Get All Shops ──────────────────────────────

UFlockGetAllShopsAction* UFlockGetAllShopsAction::GetAllShops(UObject* WorldContextObject, int32 Page, int32 Limit)
{
	UFlockGetAllShopsAction* Action = NewObject<UFlockGetAllShopsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->Page = Page;
	Action->Limit = Limit;
	return Action;
}

void UFlockGetAllShopsAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockShopPage>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetAllShopsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetAll(Page, Limit, [WeakThis](TFlockResult<FFlockShopPage> Result)
	{
		if (UFlockGetAllShopsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetAllShopsAction::Complete(const TFlockResult<FFlockShopPage>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockShopPage(), Result.Error);
	}
	SetReadyToDestroy();
}

// ──────────────────────────────── Get Shop ─────────────────────────────────

UFlockGetShopAction* UFlockGetShopAction::GetShopById(UObject* WorldContextObject, const FString& ShopId)
{
	UFlockGetShopAction* Action = NewObject<UFlockGetShopAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByName = false;
	Action->Argument = ShopId;
	return Action;
}

UFlockGetShopAction* UFlockGetShopAction::GetShopByName(UObject* WorldContextObject, const FString& Name)
{
	UFlockGetShopAction* Action = NewObject<UFlockGetShopAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByName = true;
	Action->Argument = Name;
	return Action;
}

void UFlockGetShopAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockShop>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetShopAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	auto OnDone = [WeakThis](TFlockResult<FFlockShop> Result)
	{
		if (UFlockGetShopAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	if (bByName)
	{
		Provider->GetByName(Argument, OnDone);
	}
	else
	{
		Provider->GetById(Argument, OnDone);
	}
}

void UFlockGetShopAction::Complete(const TFlockResult<FFlockShop>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockShop(), Result.Error);
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Get Shop Item ──────────────────────────────

UFlockGetShopItemAction* UFlockGetShopItemAction::GetShopItem(UObject* WorldContextObject, const FString& ShopItemId)
{
	UFlockGetShopItemAction* Action = NewObject<UFlockGetShopItemAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->ShopItemId = ShopItemId;
	return Action;
}

void UFlockGetShopItemAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockShopItem>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetShopItemAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetItem(ShopItemId, [WeakThis](TFlockResult<FFlockShopItem> Result)
	{
		if (UFlockGetShopItemAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetShopItemAction::Complete(const TFlockResult<FFlockShopItem>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockShopItem(), Result.Error);
	}
	SetReadyToDestroy();
}

// ───────────────────────────── Get Shop Items ──────────────────────────────

UFlockGetShopItemsAction* UFlockGetShopItemsAction::GetShopItems(UObject* WorldContextObject,
	const FString& ShopId, const FString& PatchId)
{
	UFlockGetShopItemsAction* Action = NewObject<UFlockGetShopItemsAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->ShopId = ShopId;
	Action->PatchId = PatchId;
	return Action;
}

void UFlockGetShopItemsAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<TArray<FFlockShopItem>>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetShopItemsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetItemsByShop(ShopId, PatchId, [WeakThis](TFlockResult<TArray<FFlockShopItem>> Result)
	{
		if (UFlockGetShopItemsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetShopItemsAction::Complete(const TFlockResult<TArray<FFlockShopItem>>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(TArray<FFlockShopItem>(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────────── Purchase ──────────────────────────────────

UFlockPurchaseAction* UFlockPurchaseAction::Purchase(UObject* WorldContextObject,
	const FString& ShopItemId, const FString& PlayerId)
{
	UFlockPurchaseAction* Action = NewObject<UFlockPurchaseAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->ShopItemId = ShopItemId;
	Action->PlayerId = PlayerId;
	return Action;
}

void UFlockPurchaseAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerInventory>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockPurchaseAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->Purchase(ShopItemId, PlayerId, [WeakThis](TFlockResult<FFlockPlayerInventory> Result)
	{
		if (UFlockPurchaseAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockPurchaseAction::Complete(const TFlockResult<FFlockPlayerInventory>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerInventory(), Result.Error);
	}
	SetReadyToDestroy();
}

// ────────────────────────── Get Player Inventory ───────────────────────────

UFlockGetPlayerInventoryAction* UFlockGetPlayerInventoryAction::GetPlayerInventory(UObject* WorldContextObject,
	const FString& PlayerId, int32 Page, int32 Limit)
{
	UFlockGetPlayerInventoryAction* Action = NewObject<UFlockGetPlayerInventoryAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->PlayerId = PlayerId;
	Action->Page = Page;
	Action->Limit = Limit;
	return Action;
}

void UFlockGetPlayerInventoryAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerInventoryPage>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetPlayerInventoryAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(*Provider, ResolveCallOrigin(WorldContextObject));
	Provider->GetPlayerInventory(PlayerId, Page, Limit, [WeakThis](TFlockResult<FFlockPlayerInventoryPage> Result)
	{
		if (UFlockGetPlayerInventoryAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetPlayerInventoryAction::Complete(const TFlockResult<FFlockPlayerInventoryPage>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPlayerInventoryPage(), Result.Error);
	}
	SetReadyToDestroy();
}
