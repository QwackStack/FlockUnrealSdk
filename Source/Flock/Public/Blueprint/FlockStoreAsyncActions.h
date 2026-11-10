// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockShopModels.h"
#include "Blueprint/FlockShopAsyncActions.h"
#include "Store/FlockStore.h"
#include "FlockStoreAsyncActions.generated.h"

class UFlockSubsystem;

/**
 * Blueprint async nodes for real money: connecting the way to pay, restoring purchases, checking the store's setup, and a web
 * checkout's own calls. Each finds Flock from its World Context when it runs and fires exactly one pin; without Flock the
 * failure pin fires saying how to initialize it, and Restore and Check Setup with no store connected say how to connect one.
 * The game instance keeps each node until its pin fires.
 *
 * Buying is the Flock Purchase node, which buys a real-money item through the connected store. Bind Flock's events (Get
 * Events) On Store Purchase Completed and On Store Purchase Not Delivered once at start to hear every delivery.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockStorePin, UFlockStore*, Store, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockStoreRestorePin, const FFlockStoreRestore&, Restore, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockStoreSetupCheckPin, const FFlockStoreSetupCheck&, SetupCheck, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockCheckoutPin, const FFlockCheckout&, Checkout, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerPurchasePin, const FFlockPlayerPurchase&, Purchase, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockPlayerPurchasePagePin, const FFlockPlayerPurchasePage&, Page, const FFlockError&, Error);

/** What every store node shares: the Flock it runs against, found from its World Context, and a way for a test to hand one in. */
UCLASS(Abstract)
class FLOCK_API UFlockStoreAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	virtual void Activate() override;

	/** Runs the node against Flock rather than the one its World Context finds (null runs it as with no Flock). */
	void ActivateWithFlockForTesting(UFlockSubsystem* Flock);

protected:
	/** Starts the node's call against Flock; null when no Flock was found. */
	virtual void Start(UFlockSubsystem* Flock) PURE_VIRTUAL(UFlockStoreAsyncAction::Start, );

	/** Fires the success pin with the call's value, or the failure pin with its error and an empty value, once; then lets the node go. */
	template <typename TValue, typename TPin>
	void Finish(const TFlockResult<TValue>& Result, TPin& OnSuccess, TPin& OnFailure)
	{
		if (Result.IsSuccess())
		{
			OnSuccess.Broadcast(Result.Value, FFlockError());
		}
		else
		{
			OnFailure.Broadcast(TValue(), Result.Error);
		}
		SetReadyToDestroy();
	}

	/** Makes a node for WorldContextObject; it is kept by that context's game instance once it runs, until its pin fires. */
	template <typename TAction>
	static TAction* Make(UObject* WorldContextObject)
	{
		TAction* Action = NewObject<TAction>();
		Action->Context = WorldContextObject;
		return Action;
	}

	/**
	 * The node's World Context: where Flock is found, and the caller its calls are logged under. Weak: the game instance keeps
	 * this node, which must not keep a level's object (and so its world) alive past a level change.
	 */
	TWeakObjectPtr<UObject> Context;
};

/**
 * Connects the game's way to pay real money, once at start: the platform's store on a phone, a web checkout through the game's
 * payment gateway on Windows, Mac and Linux, or what Options names. Reads the game's shops. Flock holds the store until Flock
 * Disconnect Store or Flock shuts down; Flock Purchase then buys real-money items through it. Fails while another store is
 * connected, without Flock's store module on a phone, and for a payment gateway named on a phone. A web checkout opens the
 * payment page in the player's default browser.
 */
UCLASS()
class FLOCK_API UFlockConnectStoreAction : public UFlockStoreAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockStorePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockStorePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Connect Store", AutoCreateRefTerm = "Options"), Category = "Flock|Store")
	static UFlockConnectStoreAction* ConnectStore(UObject* WorldContextObject, const FFlockStoreOptions& Options);

protected:
	virtual void Start(UFlockSubsystem* Flock) override;

private:
	FFlockStoreOptions Options;
};

/**
 * Has Flock deliver what the player's store account owns to the signed-in player; one delivered before grants nothing more.
 * Restored lists the items Flock holds for the player from the store's purchases, Not Delivered those it did not deliver (one
 * bought on another Flock account). On a web checkout Flock already holds what the player bought: the open checkouts are
 * read again and both lists are empty. Fails with no store connected, no player signed in, or (Cancelled) when the player
 * signs out or changes while it runs.
 */
UCLASS()
class FLOCK_API UFlockRestorePurchasesAction : public UFlockStoreAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockStoreRestorePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockStoreRestorePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Restore Purchases"), Category = "Flock|Store")
	static UFlockRestorePurchasesAction* RestorePurchases(UObject* WorldContextObject);

protected:
	virtual void Start(UFlockSubsystem* Flock) override;

};

/**
 * Checks the connected store's setup without buying anything: real-money items the store did not find or that have no product
 * id for it, and whether Flock's server can ask the store about a purchase. Is Store Ready says whether anything needs fixing.
 * On a web checkout nothing can be asked without opening a checkout: Not Checked says how to check the payment gateway. Needs
 * a signed-in player and a connected store.
 */
UCLASS()
class FLOCK_API UFlockCheckStoreSetupAction : public UFlockStoreAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockStoreSetupCheckPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockStoreSetupCheckPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Check Store Setup"), Category = "Flock|Store")
	static UFlockCheckStoreSetupAction* CheckStoreSetup(UObject* WorldContextObject);

protected:
	virtual void Start(UFlockSubsystem* Flock) override;

};

/**
 * Opens a web checkout for a real-money item as the signed-in player, for a game that shows the payment page itself (a QR code,
 * an in-game browser): the success pin carries the page's address (Payment Url) and the Purchase Id to read with Flock Get
 * Player Purchase. Nothing is paid until the payment gateway confirms. A connected web checkout does all of this inside Flock
 * Purchase. Empty Payment Provider uses the game's one active gateway; empty Return Url and Cancel Url leave the pages the
 * browser lands on to the server. Never sent again after an answer that did not come: that failure has Outcome Unknown set,
 * and Flock Get Player Purchases with the pending status lists the player's open checkouts.
 */
UCLASS()
class FLOCK_API UFlockStartCheckoutAction : public UFlockStoreAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockCheckoutPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockCheckoutPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Start Checkout", AdvancedDisplay = "PaymentProvider,ReturnUrl,CancelUrl"), Category = "Flock|Store")
	static UFlockStartCheckoutAction* StartCheckout(UObject* WorldContextObject, const FString& ShopItemId,
		const FString& PaymentProvider, const FString& ReturnUrl, const FString& CancelUrl);

protected:
	virtual void Start(UFlockSubsystem* Flock) override;

private:
	FString ShopItemId;
	FString PaymentProvider;
	FString ReturnUrl;
	FString CancelUrl;
};

/** One of the signed-in player's purchases, read now from the server: a web checkout's is pending until paid, failed or expired. */
UCLASS()
class FLOCK_API UFlockGetPlayerPurchaseAction : public UFlockStoreAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerPurchasePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerPurchasePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Purchase"), Category = "Flock|Store")
	static UFlockGetPlayerPurchaseAction* GetPlayerPurchase(UObject* WorldContextObject, const FString& PurchaseId);

protected:
	virtual void Start(UFlockSubsystem* Flock) override;

private:
	FString PurchaseId;
};

/**
 * A page of the signed-in player's purchases, newest first, read now from the server. Status is one of the Flock Purchase
 * Status nodes (pending lists the web checkouts not settled yet), or empty for every purchase; Limit is 1 to 100.
 */
UCLASS()
class FLOCK_API UFlockGetPlayerPurchasesAction : public UFlockStoreAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPlayerPurchasePagePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPlayerPurchasePagePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Purchases"), Category = "Flock|Store")
	static UFlockGetPlayerPurchasesAction* GetPlayerPurchases(UObject* WorldContextObject, const FString& Status, int32 Page = 1, int32 Limit = 100);

protected:
	virtual void Start(UFlockSubsystem* Flock) override;

private:
	FString Status;
	int32 Page = 1;
	int32 Limit = 100;
};

/**
 * Has Flock redeem a purchase the game's own store code made (Make App Store Receipt, Make Google Play Receipt), as the signed-in
 * player: Flock asks the store about it and hands the item over, completing with the purchase as Flock Purchase does. Safe to
 * send again: one store purchase makes one Flock purchase, and a repeat answers with what the first delivered. Leave the
 * store's purchase unfinished until the success pin fires; Outcome Unknown on the failure pin means send the same receipt
 * again later. A connected store (Flock Connect Store) does all of this for its own purchases.
 */
UCLASS()
class FLOCK_API UFlockRedeemStoreReceiptAction : public UFlockStoreAsyncAction
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockPurchaseResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockPurchaseResultPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Redeem Store Receipt"), Category = "Flock|Store")
	static UFlockRedeemStoreReceiptAction* RedeemStoreReceipt(UObject* WorldContextObject, const FString& ShopItemId, const FFlockStoreReceipt& Receipt);

protected:
	virtual void Start(UFlockSubsystem* Flock) override;

private:
	FString ShopItemId;
	FFlockStoreReceipt Receipt;
};
