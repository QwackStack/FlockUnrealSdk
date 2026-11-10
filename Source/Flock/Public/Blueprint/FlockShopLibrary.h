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

	/** True when the purchase is paid, so what it grants is the player's. A pending real-money purchase has handed nothing over. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Purchase Paid"), Category = "Flock|Shop")
	static bool IsPurchasePaid(const FFlockPurchaseResult& PurchaseResult) { return PurchaseResult.IsPaid(); }

	/** Purchase statuses as the server spells them, to compare a result's Status against rather than typing them. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Purchase Status Pending"), Category = "Flock|Shop")
	static FString PurchaseStatusPending() { return FlockPurchaseStatuses::Pending; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Purchase Status Paid"), Category = "Flock|Shop")
	static FString PurchaseStatusPaid() { return FlockPurchaseStatuses::Paid; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Purchase Status Failed"), Category = "Flock|Shop")
	static FString PurchaseStatusFailed() { return FlockPurchaseStatuses::Failed; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Purchase Status Expired"), Category = "Flock|Shop")
	static FString PurchaseStatusExpired() { return FlockPurchaseStatuses::Expired; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Purchase Status Refunded"), Category = "Flock|Shop")
	static FString PurchaseStatusRefunded() { return FlockPurchaseStatuses::Refunded; }

	/** The consume counterpart: true when consuming the entry moved currency and reported the new balances. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Has Wallet (Consume)"), Category = "Flock|Shop")
	static bool ConsumeHasWallet(const FFlockConsumeResult& ConsumeResult) { return ConsumeResult.HasWallet(); }

	/**
	 * The item's product id in Store (one of the Flock Payment Provider nodes), exactly as set in the Flock dashboard. Matches the
	 * store's name with its letter case, which a map's own Find does not. False when the item has none there.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Find Store Product Id"), Category = "Flock|Shop")
	static bool FindStoreProductId(const FFlockShopItem& Item, const FString& Store, FString& ProductId)
	{
		return Item.FindStoreProductId(Store, ProductId);
	}

	/** Store and payment gateway names as the server spells them. Apple App Store and Google Play are the keys of an item's Store Product Ids; a web checkout needs no product id. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Payment Provider Apple App Store"), Category = "Flock|Shop")
	static FString PaymentProviderAppleAppStore() { return FlockPaymentProviders::AppleAppStore; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Payment Provider Google Play"), Category = "Flock|Shop")
	static FString PaymentProviderGooglePlay() { return FlockPaymentProviders::GooglePlay; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Payment Provider Stripe"), Category = "Flock|Shop")
	static FString PaymentProviderStripe() { return FlockPaymentProviders::Stripe; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Payment Provider Tap"), Category = "Flock|Shop")
	static FString PaymentProviderTap() { return FlockPaymentProviders::Tap; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Payment Provider Moyasar"), Category = "Flock|Shop")
	static FString PaymentProviderMoyasar() { return FlockPaymentProviders::Moyasar; }

	/** A currency's balance in the wallet, or 0 when it holds none (Wallet Holds tells the two apart). Codes match with their case. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Wallet Balance Of"), Category = "Flock|Shop")
	static int64 WalletBalanceOf(const FFlockWallet& Wallet, const FString& CurrencyCode) { return Wallet.BalanceOf(CurrencyCode); }

	/** Whether the wallet holds a balance in this currency at all, a zero one included. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Wallet Holds"), Category = "Flock|Shop")
	static bool WalletHolds(const FFlockWallet& Wallet, const FString& CurrencyCode) { return Wallet.Holds(CurrencyCode); }

	/** Whether Purchase can pay for this item from the wallet; false for a real-money item, which a store sells. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Can Afford"), Category = "Flock|Shop")
	static bool CanAfford(const FFlockWallet& Wallet, const FFlockShopItem& Item) { return Wallet.CanAfford(Item); }

	/** The balances a purchase left, by currency code; empty when it moved no currency (Has Wallet is false). */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Purchase Balances"), Category = "Flock|Shop")
	static FFlockWallet PurchaseBalances(const FFlockPurchaseResult& PurchaseResult) { return FFlockWallet::From(PurchaseResult.Wallet); }

	/** The balances a consume left, by currency code; empty when it granted no currency (Has Wallet is false). */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Consume Balances"), Category = "Flock|Shop")
	static FFlockWallet ConsumeBalances(const FFlockConsumeResult& ConsumeResult) { return FFlockWallet::From(ConsumeResult.Wallet); }

	/** How many unused entries of this shop item the counts hold; 0 when none. The id matches with its case. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Owned Count Of"), Category = "Flock|Shop")
	static int32 OwnedCountOf(const FFlockOwnedItemCounts& Counts, const FString& ShopItemId) { return Counts.CountOf(ShopItemId); }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Inventory Status Unused"), Category = "Flock|Shop")
	static FString PlayerInventoryStatusUnused() { return FlockPlayerInventoryStatuses::Unused; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Inventory Status Used"), Category = "Flock|Shop")
	static FString PlayerInventoryStatusUsed() { return FlockPlayerInventoryStatuses::Used; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Inventory Status Revoked"), Category = "Flock|Shop")
	static FString PlayerInventoryStatusRevoked() { return FlockPlayerInventoryStatuses::Revoked; }

	/** Item kinds as the server spells them, to compare an item's Type against rather than typing them. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Shop Item Type Standard"), Category = "Flock|Shop")
	static FString ShopItemTypeStandard() { return FlockShopItemTypes::Standard; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Shop Item Type Currency Pack"), Category = "Flock|Shop")
	static FString ShopItemTypeCurrencyPack() { return FlockShopItemTypes::CurrencyPack; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Shop Item Type Consumable"), Category = "Flock|Shop")
	static FString ShopItemTypeConsumable() { return FlockShopItemTypes::Consumable; }
};
