// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockShopModels.h"
#include "FlockShopLibrary.generated.h"

/**
 * Pure Blueprint accessors over the shop models.
 *
 * These exist because a reward's kind is a **string the server owns**, not an enum: without them a graph
 * would have to compare against a hand-typed "currency" literal, and a typo would read as "not a currency
 * reward" and silently skip the grant. The type constant is surfaced as a node so a graph and C++ compare
 * against the same value. Each one delegates straight to the struct or the constant it reads, so the two
 * cannot drift — pinned by Shop.Library.CppParity.
 */
UCLASS()
class FLOCK_API UFlockShopLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * True for a currency grant — the only reward kind that exists today.
	 *
	 * Branch on this rather than comparing Type to a literal. A kind the server adds later answers false
	 * here and stays readable through Type, which is the whole reason the field is not an enum.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Currency Reward"), Category = "Flock|Shop")
	static bool IsCurrencyReward(const FFlockShopItemReward& Reward) { return Reward.IsCurrency(); }

	/**
	 * The wire spelling of the currency reward kind ("currency").
	 *
	 * For a graph that switches on Type across several kinds rather than asking one yes/no question.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Currency Reward Type"), Category = "Flock|Shop")
	static FString CurrencyRewardType() { return FlockShopItemRewardTypes::Currency; }

	/**
	 * True when a purchase created a row the player now owns.
	 *
	 * A purchase does not always produce one: an item that hands its contents over outright reports its
	 * grant in Granted and leaves Inventory empty. Test this before reading the row, rather than treating
	 * a default-constructed struct as ownership.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Has Inventory Row"), Category = "Flock|Shop")
	static bool HasInventoryRow(const FFlockPurchaseResult& PurchaseResult) { return PurchaseResult.HasInventoryRow(); }

	/**
	 * True when the server reported the wallet alongside a purchase — it does so only when currency moved.
	 *
	 * Where this is false the balance is unchanged, not unknown, so there is nothing to re-read.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Has Wallet"), Category = "Flock|Shop")
	static bool PurchaseHasWallet(const FFlockPurchaseResult& PurchaseResult) { return PurchaseResult.HasWallet(); }

	/** The consume counterpart: true when consuming the entry moved currency and reported the new balances. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Has Wallet (Consume)"), Category = "Flock|Shop")
	static bool ConsumeHasWallet(const FFlockConsumeResult& ConsumeResult) { return ConsumeResult.HasWallet(); }
};
