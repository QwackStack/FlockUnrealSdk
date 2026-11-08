// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockShopModels.h"
#include "FlockShopAsyncActions.generated.h"

/**
 * Blueprint async nodes for the shop: catalog reads, purchase, and player inventory. Each resolves the
 * SDK from its world context on Activate and fires exactly one pin; when the SDK is unavailable the
 * failure pin fires with a Validation error. Wider than the (pre-codegen) public C++ intent on purpose,
 * matching the config/game nodes — a graph needs the full surface to build a shop UI.
 *
 * An empty Player Id means "the signed-in player" (Purchase, Get Player Inventory). Read a shop/item's
 * free-form data off the FFlockJsonData handle (Data / Stats) with the UFlockJsonDataLibrary nodes.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockShopPagePin, const FFlockShopPage&, Page, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockShopPin, const FFlockShop&, Shop, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockShopItemPin, const FFlockShopItem&, Item, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockShopItemListPin, const TArray<FFlockShopItem>&, Items, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPurchaseResultPin, const FFlockPurchaseResult&, PurchaseResult, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockConsumeResultPin, const FFlockConsumeResult&, ConsumeResult, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerInventoryPagePin, const FFlockPlayerInventoryPage&, Page, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockOwnedItemCountsPin, const FFlockOwnedItemCounts&, Counts, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockOwnsShopItemPin, bool, bOwns, const FFlockError&, Error);

/** Fetches a page of shops. */
UCLASS()
class FLOCK_API UFlockGetAllShopsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockShopPagePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockShopPagePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get All Shops"), Category = "Flock|Shop")
	static UFlockGetAllShopsAction* GetAllShops(UObject* WorldContextObject, int32 Page = 1, int32 Limit = 100);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockShopPage>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	int32 Page = 1;
	int32 Limit = 100;
};

/** Fetches a single shop by id or by name. */
UCLASS()
class FLOCK_API UFlockGetShopAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockShopPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockShopPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Shop By Id"), Category = "Flock|Shop")
	static UFlockGetShopAction* GetShopById(UObject* WorldContextObject, const FString& ShopId);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Shop By Name"), Category = "Flock|Shop")
	static UFlockGetShopAction* GetShopByName(UObject* WorldContextObject, const FString& Name);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockShop>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	bool bByName = false;
	FString ShopIdOrName;
};

/** Fetches a single shop item by id. */
UCLASS()
class FLOCK_API UFlockGetShopItemAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockShopItemPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockShopItemPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Shop Item"), Category = "Flock|Shop")
	static UFlockGetShopItemAction* GetShopItem(UObject* WorldContextObject, const FString& ShopItemId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockShopItem>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString ShopItemId;
};

/** Fetches the items in a shop (optionally as of a patch; empty patch id = current). */
UCLASS()
class FLOCK_API UFlockGetShopItemsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockShopItemListPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockShopItemListPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Shop Items"), Category = "Flock|Shop")
	static UFlockGetShopItemsAction* GetShopItems(UObject* WorldContextObject, const FString& ShopId, const FString& PatchId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<TArray<FFlockShopItem>>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString ShopId;
	FString PatchId;
};

/**
 * Buys an item for a player (empty player id = the signed-in player).
 *
 * The success pin carries the whole purchase result, not just the inventory row: break it for Inventory,
 * Granted and Wallet. An item that grants its contents outright leaves Inventory empty — check its Id
 * before treating it as something the player now owns.
 */
UCLASS()
class FLOCK_API UFlockPurchaseAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPurchaseResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPurchaseResultPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Purchase", AdvancedDisplay = "PlayerId"), Category = "Flock|Shop")
	static UFlockPurchaseAction* Purchase(UObject* WorldContextObject, const FString& ShopItemId, const FString& PlayerId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPurchaseResult>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString ShopItemId;
	FString PlayerId;
};

/**
 * Consumes an owned inventory entry, granting whatever it carries.
 *
 * Money-moving, like Purchase: an ambiguous failure fires the failure pin rather than quietly re-sending,
 * because a second attempt would grant twice. Take the Inventory Id from a Get Player Inventory row or
 * from a purchase result.
 */
UCLASS()
class FLOCK_API UFlockConsumeInventoryAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockConsumeResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockConsumeResultPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Consume Inventory Item"), Category = "Flock|Shop")
	static UFlockConsumeInventoryAction* ConsumeInventoryItem(UObject* WorldContextObject, const FString& InventoryId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockConsumeResult>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString InventoryId;
};

/** Fetches a page of a player's owned items (empty player id = the signed-in player). */
UCLASS()
class FLOCK_API UFlockGetPlayerInventoryAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerInventoryPagePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerInventoryPagePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Inventory", AdvancedDisplay = "PlayerId"), Category = "Flock|Shop")
	static UFlockGetPlayerInventoryAction* GetPlayerInventory(UObject* WorldContextObject, const FString& PlayerId,
		int32 Page = 1, int32 Limit = 100);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerInventoryPage>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString PlayerId;
	int32 Page = 1;
	int32 Limit = 100;
};

/**
 * Uses up one unused entry of a shop item the signed-in player holds, granting what it holds. Reads the inventory to find
 * one; an entry used meanwhile on another device is passed over for the next. The failure pin fires when the player holds
 * none, when nobody is signed in, or with Outcome Unknown set when no answer said whether it was used.
 */
UCLASS()
class FLOCK_API UFlockConsumeOneAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockConsumeResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockConsumeResultPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Consume One"), Category = "Flock|Shop")
	static UFlockConsumeOneAction* ConsumeOne(UObject* WorldContextObject, const FString& ShopItemId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockConsumeResult>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString ShopItemId;
};

/**
 * How many unused entries of each shop item the signed-in player holds, from every page of the inventory. Read it once for
 * a screen of items and ask Owned Count Of per item.
 */
UCLASS()
class FLOCK_API UFlockGetOwnedItemCountsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockOwnedItemCountsPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockOwnedItemCountsPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Owned Item Counts"), Category = "Flock|Shop")
	static UFlockGetOwnedItemCountsAction* GetOwnedItemCounts(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockOwnedItemCounts>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/** Whether the signed-in player holds an unused entry of a shop item. Reads the whole inventory. */
UCLASS()
class FLOCK_API UFlockOwnsShopItemAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockOwnsShopItemPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockOwnsShopItemPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Owns Shop Item"), Category = "Flock|Shop")
	static UFlockOwnsShopItemAction* OwnsShopItem(UObject* WorldContextObject, const FString& ShopItemId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<bool>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString ShopItemId;
};
