// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"

#if !UE_BUILD_SHIPPING

#include "FlockLogger.h"
#include "Models/FlockShopModels.h"

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

#endif // !UE_BUILD_SHIPPING
