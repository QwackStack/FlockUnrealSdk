// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockShopAsyncActions.h"
#include "Blueprint/FlockBlueprintCallOrigin.h"
#include "Blueprint/FlockBlueprintNodeLife.h"
#include "Blueprint/FlockBlueprintShop.h"
#include "Http/FlockErrorHints.h"

#include "FlockSubsystem.h"
#include "Providers/FlockShopProvider.h"

namespace
{
	FFlockShopProvider* ResolveShop(UObject* WorldContextObject, FFlockError& OutError)
	{
		return FlockBlueprintShop::ShopOf(UFlockSubsystem::Get(WorldContextObject), OutError);
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
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockShopPage>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetAllShopsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
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
	Action->ShopIdOrName = ShopId;
	return Action;
}

UFlockGetShopAction* UFlockGetShopAction::GetShopByName(UObject* WorldContextObject, const FString& Name)
{
	UFlockGetShopAction* Action = NewObject<UFlockGetShopAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->bByName = true;
	Action->ShopIdOrName = Name;
	return Action;
}

void UFlockGetShopAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockShop>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetShopAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	auto OnDone = [WeakThis](TFlockResult<FFlockShop> Result)
	{
		if (UFlockGetShopAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	};

	if (bByName)
	{
		Provider->GetByName(ShopIdOrName, OnDone);
	}
	else
	{
		Provider->GetById(ShopIdOrName, OnDone);
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
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockShopItem>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetShopItemAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
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
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<TArray<FFlockShopItem>>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetShopItemsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
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
	ActivateWithFlockForTesting(UFlockSubsystem::Get(WorldContextObject.Get()));
}

void UFlockPurchaseAction::ActivateWithFlockForTesting(UFlockSubsystem* Flock)
{
	// Kept by the game instance until its pin fires (Complete lets it go).
	RegisterWithGameInstance(FlockBlueprintNodeLife::GameInstanceOf(WorldContextObject.Get()));
	FFlockError Error;
	FFlockShopProvider* Provider = FlockBlueprintShop::ShopOf(Flock, Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPurchaseResult>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockPurchaseAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->Purchase(ShopItemId, PlayerId, [WeakThis](TFlockResult<FFlockPurchaseResult> Result)
	{
		if (UFlockPurchaseAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockPurchaseAction::Complete(const TFlockResult<FFlockPurchaseResult>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockPurchaseResult(), Result.Error);
	}
	SetReadyToDestroy();
}

// ─────────────────────────── Consume Inventory Item ────────────────────────

UFlockConsumeInventoryAction* UFlockConsumeInventoryAction::ConsumeInventoryItem(UObject* WorldContextObject,
	const FString& InventoryId)
{
	UFlockConsumeInventoryAction* Action = NewObject<UFlockConsumeInventoryAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->InventoryId = InventoryId;
	return Action;
}

void UFlockConsumeInventoryAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockConsumeResult>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockConsumeInventoryAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->Consume(InventoryId, [WeakThis](TFlockResult<FFlockConsumeResult> Result)
	{
		if (UFlockConsumeInventoryAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockConsumeInventoryAction::Complete(const TFlockResult<FFlockConsumeResult>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockConsumeResult(), Result.Error);
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
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockPlayerInventoryPage>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetPlayerInventoryAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
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

// ────────────────────────────── Consume One ──────────────────────────────

UFlockConsumeOneAction* UFlockConsumeOneAction::ConsumeOne(UObject* WorldContextObject, const FString& ShopItemId)
{
	UFlockConsumeOneAction* Action = NewObject<UFlockConsumeOneAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->ShopItemId = ShopItemId;
	return Action;
}

void UFlockConsumeOneAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockConsumeResult>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockConsumeOneAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->ConsumeOne(ShopItemId, [WeakThis](TFlockResult<FFlockConsumeResult> Result)
	{
		if (UFlockConsumeOneAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockConsumeOneAction::Complete(const TFlockResult<FFlockConsumeResult>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockConsumeResult(), Result.Error);
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Get Owned Item Counts ──────────────────────────────

UFlockGetOwnedItemCountsAction* UFlockGetOwnedItemCountsAction::GetOwnedItemCounts(UObject* WorldContextObject)
{
	UFlockGetOwnedItemCountsAction* Action = NewObject<UFlockGetOwnedItemCountsAction>();
	Action->WorldContextObject = WorldContextObject;
	return Action;
}

void UFlockGetOwnedItemCountsAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<FFlockOwnedItemCounts>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockGetOwnedItemCountsAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->GetOwnedItemCounts([WeakThis](TFlockResult<FFlockOwnedItemCounts> Result)
	{
		if (UFlockGetOwnedItemCountsAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockGetOwnedItemCountsAction::Complete(const TFlockResult<FFlockOwnedItemCounts>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(FFlockOwnedItemCounts(), Result.Error);
	}
	SetReadyToDestroy();
}

// ────────────────────────────── Owns Shop Item ──────────────────────────────

UFlockOwnsShopItemAction* UFlockOwnsShopItemAction::OwnsShopItem(UObject* WorldContextObject, const FString& ShopItemId)
{
	UFlockOwnsShopItemAction* Action = NewObject<UFlockOwnsShopItemAction>();
	Action->WorldContextObject = WorldContextObject;
	Action->ShopItemId = ShopItemId;
	return Action;
}

void UFlockOwnsShopItemAction::Activate()
{
	FFlockError Error;
	FFlockShopProvider* Provider = ResolveShop(WorldContextObject.Get(), Error);
	if (!Provider)
	{
		Complete(TFlockResult<bool>::Fail(Error));
		return;
	}

	TWeakObjectPtr<UFlockOwnsShopItemAction> WeakThis(this);
	const FFlockCallOriginScope OriginScope(FlockBlueprintCallOrigin::Of(WorldContextObject.Get()));
	Provider->Owns(ShopItemId, [WeakThis](TFlockResult<bool> Result)
	{
		if (UFlockOwnsShopItemAction* Self = WeakThis.Get())
		{
			Self->Complete(Result);
		}
	});
}

void UFlockOwnsShopItemAction::Complete(const TFlockResult<bool>& Result)
{
	if (Result.bSuccess)
	{
		OnSuccess.Broadcast(Result.Value, FFlockError());
	}
	else
	{
		OnFailure.Broadcast(false, Result.Error);
	}
	SetReadyToDestroy();
}
