// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Store/FlockStore.h"
#include "FlockStoreLibrary.generated.h"

class UFlockSubsystem;

/**
 * Blueprint nodes for the way to pay real money Flock holds (Flock Connect Store connects it): its price, the store itself,
 * disconnecting it, the setup check's verdict, and the reasons a store did not take a payment. Each finds the store from its
 * World Context; with none connected, Flock Get Store Price fails saying so and how to connect one.
 *
 * Buying is the Flock Purchase node: once a store is connected it buys a real-money item through it. Its failure pin's Error
 * carries Store Failure Reason (compare it with the Flock Store Failure Reason nodes) and Outcome Unknown (the purchase may
 * have been paid: read the wallet and inventory before buying again).
 */
UCLASS()
class FLOCK_API UFlockStoreLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * A real-money item's price to show this player. On a phone, as the store shows it, in their currency and with their taxes
	 * (Found In The Store is false when the store did not find the item's product); on a web checkout, the item's own price,
	 * which is what the payment gateway charges. False, with Error saying why and how to fix it, when no store is connected,
	 * for an item not in the game's shops when the store connected, one with no product id for this store, and on a web
	 * checkout one priced in a game currency.
	 */
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Store Price"), Category = "Flock|Store")
	static bool GetStorePrice(UObject* WorldContextObject, const FString& ShopItemId, FFlockStorePrice& Price, FFlockError& Error);

	/** Get Store Price against Flock rather than the one a World Context finds (null runs it as with no Flock); what the node runs. */
	static bool GetStorePriceWithFlockForTesting(UFlockSubsystem* Flock, const FString& ShopItemId, FFlockStorePrice& Price, FFlockError& Error);

	/** The way to pay real money Flock holds (from the moment Flock Connect Store starts connecting), or none. */
	UFUNCTION(BlueprintPure, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Store"), Category = "Flock|Store")
	static UFlockStore* GetStore(UObject* WorldContextObject);

	/** Get Store against Flock rather than the one a World Context finds; what the node runs. */
	static UFlockStore* GetStoreWithFlockForTesting(const UFlockSubsystem* Flock);

	/**
	 * Disconnects the store Flock holds: Flock Purchase no longer buys real-money items through it, and a purchase still waiting
	 * ends Cancelled. Does nothing when none is connected. Connect again with Flock Connect Store.
	 */
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Disconnect Store"), Category = "Flock|Store")
	static void DisconnectStore(UObject* WorldContextObject);

	/** Disconnect Store against Flock rather than the one a World Context finds; what the node runs. */
	static void DisconnectStoreWithFlockForTesting(const UFlockSubsystem* Flock);

	/**
	 * True when the setup check found nothing to fix: every real-money item found in the store, and Flock's server able to ask it.
	 * On a web checkout, true when nothing was found to fix; Not Checked says how to check the gateway by hand.
	 */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Store Ready"), Category = "Flock|Store")
	static bool IsStoreReady(const FFlockStoreSetupCheck& SetupCheck) { return SetupCheck.IsReady(); }

	/** True when a purchase read with Flock Get Player Purchase is paid, so what it grants is the player's. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Player Purchase Paid"), Category = "Flock|Store")
	static bool IsPlayerPurchasePaid(const FFlockPlayerPurchase& Purchase) { return Purchase.IsPaid(); }

	/** True while a purchase read with Flock Get Player Purchase waits for its payment (a web checkout not yet paid, failed or expired). */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Is Player Purchase Pending"), Category = "Flock|Store")
	static bool IsPlayerPurchasePending(const FFlockPlayerPurchase& Purchase) { return Purchase.IsPending(); }

	/** A purchase the App Store made, by its transaction id exactly as StoreKit gave it, for Flock Redeem Store Receipt. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Make App Store Receipt"), Category = "Flock|Store")
	static FFlockStoreReceipt MakeAppStoreReceipt(const FString& TransactionId) { return FFlockStoreReceipt::FromAppleAppStore(TransactionId); }

	/** A purchase Google Play made, by its product id and purchase token exactly as Google Play gave them, for Flock Redeem Store Receipt. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Make Google Play Receipt"), Category = "Flock|Store")
	static FFlockStoreReceipt MakeGooglePlayReceipt(const FString& ProductId, const FString& PurchaseToken)
	{
		return FFlockStoreReceipt::FromGooglePlay(ProductId, PurchaseToken);
	}

	/** Why a store did not take a payment, as Flock spells it, to compare a Flock Error's Store Failure Reason against. */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Cancelled"), Category = "Flock|Store")
	static FString StoreFailureReasonCancelled() { return FlockStoreFailureReasons::Cancelled; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Payment Declined"), Category = "Flock|Store")
	static FString StoreFailureReasonPaymentDeclined() { return FlockStoreFailureReasons::PaymentDeclined; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Already Owned"), Category = "Flock|Store")
	static FString StoreFailureReasonAlreadyOwned() { return FlockStoreFailureReasons::AlreadyOwned; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Already Pending"), Category = "Flock|Store")
	static FString StoreFailureReasonAlreadyPending() { return FlockStoreFailureReasons::AlreadyPending; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Product Unavailable"), Category = "Flock|Store")
	static FString StoreFailureReasonProductUnavailable() { return FlockStoreFailureReasons::ProductUnavailable; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Store Unavailable"), Category = "Flock|Store")
	static FString StoreFailureReasonStoreUnavailable() { return FlockStoreFailureReasons::StoreUnavailable; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Not Signed In To The Store"), Category = "Flock|Store")
	static FString StoreFailureReasonNotSignedInToTheStore() { return FlockStoreFailureReasons::NotSignedInToTheStore; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Expired"), Category = "Flock|Store")
	static FString StoreFailureReasonExpired() { return FlockStoreFailureReasons::Expired; }

	UFUNCTION(BlueprintPure, meta = (DisplayName = "Flock Store Failure Reason Unknown"), Category = "Flock|Store")
	static FString StoreFailureReasonUnknown() { return FlockStoreFailureReasons::Unknown; }
};
