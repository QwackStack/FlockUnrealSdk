// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "FlockLogger.h"
#include "Models/FlockShopModels.h"
#include "Store/FlockStore.h"

/**
 * The self-test's purchase narration, lifted out of the completion lambda so it can be exercised
 * without a backend.
 *
 * It is here rather than inline because the three states it distinguishes — a purchase with no
 * inventory row, one that granted nothing, one that moved no currency — were unreachable live while
 * reward granting was failing server-side: no successful purchase result had ever been serialized, so
 * inline, the first sight of this output would have been the first time the backend fix worked, and a
 * wrong branch would have read as a backend problem. The branches are pinned by tests; the live run
 * now only has to confirm them.
 *
 * Narration only — it makes no decisions the SDK acts on. Everything goes to Info: a purchase that
 * granted nothing is an ordinary outcome, and logging it louder would train people to ignore the log.
 */
inline void FlockNarratePurchaseResult(const FFlockPurchaseResult& Result, IFlockLogger& Logger)
{
	// The row is optional: an item that hands its contents over outright creates nothing to own and
	// reports the grant instead. Named rather than printed as an empty id, so the two cases are told
	// apart at a glance instead of looking like a truncated line.
	Logger.LogInfo(FString::Printf(
		TEXT("Self-test: purchase -> purchase_id=%s item_type=%s; inventory entry %s"),
		*Result.PurchaseId, *Result.ItemType,
		!Result.HasInventoryRow()
			? TEXT("(none - item granted its contents outright)")
			: *Result.Inventory.Id));

	// The grant list is the half no fixture can prove. The request is verified live every run, but the
	// response shape is derived from the spec, so a non-empty list here is the evidence it is right.
	if (Result.Granted.Num() == 0)
	{
		Logger.LogInfo(TEXT("Self-test: purchase granted nothing (a plain inventory item)."));
	}
	for (const FFlockShopItemReward& Reward : Result.Granted)
	{
		// Type is printed verbatim rather than mapped: it is an open set the server owns, and a kind
		// added later should show up in the log as itself rather than as "unknown".
		Logger.LogInfo(FString::Printf(TEXT("Self-test:   granted %d x %s (type=%s)"),
			Reward.Amount, *Reward.Code, *Reward.Type));
	}

	Logger.LogInfo(!Result.HasWallet()
		? FString(TEXT("Self-test: no wallet in the response - this purchase moved no currency."))
		: FString::Printf(TEXT("Self-test: wallet after the purchase -> player data row %s"),
			*Result.Wallet.Id));
}

/**
 * The payment leg's line for one real-money item: its name, its Flock price and every store's product id for it, by store name in
 * order, so a run says which store each item can be sold through. An item with no product id says so: it is sold only through a
 * web checkout.
 */
inline void FlockNarrateRealMoneyItem(const FFlockShopItem& Item, IFlockLogger& Logger)
{
	TArray<FString> Stores;
	Item.StoreProductIds.GetKeys(Stores);
	Stores.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	TArray<FString> Ids;
	for (const FString& Store : Stores)
	{
		Ids.Add(FString::Printf(TEXT("%s=%s"), *Store, *Item.StoreProductIds[Store]));
	}
	Logger.LogInfo(FString::Printf(TEXT("Self-test: real-money item '%s' (%s) %d %s (in the currency's smallest unit); store product ids: %s"), *Item.Name,
		*Item.Id, Item.Price, *Item.Currency, Ids.Num() > 0 ? *FString::Join(Ids, TEXT(", ")) : TEXT("(none - sold through a web checkout only)")));
}

/**
 * The payment leg's narration of a store setup check: whether it is ready, what could not be checked and how to check it, each
 * item the store did not find or has no product id for, and why Flock's server could not ask the store, with its fix. Info only:
 * a setup not ready is what the check exists to report.
 */
inline void FlockNarrateStoreSetupCheck(const FFlockStoreSetupCheck& Check, IFlockLogger& Logger)
{
	Logger.LogInfo(FString::Printf(TEXT("Self-test: store setup check -> %s"), Check.IsReady() ? TEXT("ready") : TEXT("not ready")));
	if (!Check.NotChecked.IsEmpty())
	{
		Logger.LogInfo(FString::Printf(TEXT("Self-test:   not checked: %s"), *Check.NotChecked));
	}
	for (const FFlockShopItem& Item : Check.NotFoundInTheStore)
	{
		Logger.LogInfo(FString::Printf(TEXT("Self-test:   the store did not find '%s' (%s): check its product id and that the product is active in the store"),
			*Item.Name, *Item.Id));
	}
	for (const FFlockShopItem& Item : Check.MissingProductId)
	{
		Logger.LogInfo(FString::Printf(TEXT("Self-test:   '%s' (%s) has no product id for this store: enter it on the item in the Flock dashboard"),
			*Item.Name, *Item.Id));
	}
	if (Check.bHasServerProblem)
	{
		Logger.LogInfo(FString::Printf(TEXT("Self-test:   Flock's server cannot ask the store about purchases: %s"), *Check.ServerProblem.ReasonAndFix()));
	}
	else if (Check.bServerCanCheckPurchases)
	{
		Logger.LogInfo(TEXT("Self-test:   Flock's server can ask the store about purchases with the game's keys"));
	}
}

#endif // !UE_BUILD_SHIPPING
