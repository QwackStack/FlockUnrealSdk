// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/FlockShopAsyncActions.h"
#include "Blueprint/FlockStoreAsyncActions.h"
#include "FlockEvents.h"
#include "Http/FlockError.h"
#include "Models/FlockShopModels.h"
#include "Store/FlockStore.h"
#include "UObject/Object.h"
#include "UObject/StrongObjectPtr.h"
#include "FlockStoreNodeTestListener.generated.h"

/**
 * Bind target for the store node tests: a node's pins and Flock's events are dynamic delegates, which bind only to UFUNCTIONs.
 * Each handler records which pin fired, in order, with what it carried, so a test reads which pin fired and how often. Not
 * guarded by WITH_AUTOMATION_TESTS: UHT registers it unconditionally.
 */
UCLASS()
class UFlockStoreNodeTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Fired;
	/** The error of the pin that fired last; Errors holds every pin's, in order. */
	FFlockError Error;
	TArray<FFlockError> Errors;

	UPROPERTY()
	TObjectPtr<UFlockStore> Store;

	FFlockStoreRestore Restore;
	FFlockStoreSetupCheck SetupCheck;
	FFlockCheckout Checkout;
	FFlockPlayerPurchase Purchase;
	FFlockPlayerPurchasePage Page;
	FFlockPurchaseResult PurchaseResult;

	/** What Flock's events hub raised: "<item>:<purchase>" for a delivery, the product id for a purchase not delivered. */
	TArray<FString> Completed;
	TArray<FFlockUndeliveredPurchase> NotDelivered;

	FString FiredText() const { return FString::Join(Fired, TEXT(",")); }

	UFUNCTION() void StoreSucceeded(UFlockStore* InStore, const FFlockError& InError) { Store = InStore; Take(TEXT("success"), InError); }
	UFUNCTION() void StoreFailed(UFlockStore* InStore, const FFlockError& InError) { Store = InStore; Take(TEXT("failure"), InError); }
	UFUNCTION() void RestoreSucceeded(const FFlockStoreRestore& In, const FFlockError& InError) { Restore = In; Take(TEXT("success"), InError); }
	UFUNCTION() void RestoreFailed(const FFlockStoreRestore& In, const FFlockError& InError) { Restore = In; Take(TEXT("failure"), InError); }
	UFUNCTION() void SetupSucceeded(const FFlockStoreSetupCheck& In, const FFlockError& InError) { SetupCheck = In; Take(TEXT("success"), InError); }
	UFUNCTION() void SetupFailed(const FFlockStoreSetupCheck& In, const FFlockError& InError) { SetupCheck = In; Take(TEXT("failure"), InError); }
	UFUNCTION() void CheckoutSucceeded(const FFlockCheckout& In, const FFlockError& InError) { Checkout = In; Take(TEXT("success"), InError); }
	UFUNCTION() void CheckoutFailed(const FFlockCheckout& In, const FFlockError& InError) { Checkout = In; Take(TEXT("failure"), InError); }
	UFUNCTION() void PurchaseRead(const FFlockPlayerPurchase& In, const FFlockError& InError) { Purchase = In; Take(TEXT("success"), InError); }
	UFUNCTION() void PurchaseReadFailed(const FFlockPlayerPurchase& In, const FFlockError& InError) { Purchase = In; Take(TEXT("failure"), InError); }
	UFUNCTION() void PageRead(const FFlockPlayerPurchasePage& In, const FFlockError& InError) { Page = In; Take(TEXT("success"), InError); }
	UFUNCTION() void PageReadFailed(const FFlockPlayerPurchasePage& In, const FFlockError& InError) { Page = In; Take(TEXT("failure"), InError); }
	UFUNCTION() void Bought(const FFlockPurchaseResult& In, const FFlockError& InError) { PurchaseResult = In; Take(TEXT("success"), InError); }
	UFUNCTION() void NotBought(const FFlockPurchaseResult& In, const FFlockError& InError) { PurchaseResult = In; Take(TEXT("failure"), InError); }

	UFUNCTION() void HeardCompleted(const FFlockShopItem& Item, const FFlockPurchaseResult& Result) { Completed.Add(Item.Id + TEXT(":") + Result.PurchaseId); }
	UFUNCTION() void HeardNotDelivered(const FFlockUndeliveredPurchase& In) { NotDelivered.Add(In); }

	/** Binds the two store events on Flock's events hub, as a Blueprint does once at start. */
	void Hear(UFlockEvents* Events)
	{
		Events->OnStorePurchaseCompleted.AddDynamic(this, &UFlockStoreNodeTestListener::HeardCompleted);
		Events->OnStorePurchaseNotDelivered.AddDynamic(this, &UFlockStoreNodeTestListener::HeardNotDelivered);
	}

private:
	void Take(const TCHAR* Pin, const FFlockError& InError)
	{
		Fired.Add(Pin);
		Error = InError;
		Errors.Add(InError);
	}
};

/** A test's store nodes: the listener their pins fire into, and every node kept from garbage collection while the test runs. */
struct FFlockStoreNodes
{
	TStrongObjectPtr<UFlockStoreNodeTestListener> Listener{ NewObject<UFlockStoreNodeTestListener>() };
	TArray<TStrongObjectPtr<UObject>> Kept;

	UFlockStoreNodeTestListener* operator->() const { return Listener.Get(); }

	template <typename TAction>
	TAction* Keep(TAction* Action)
	{
		Kept.Emplace(Action);
		return Action;
	}

	UFlockConnectStoreAction* Connect(UFlockConnectStoreAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::StoreSucceeded);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::StoreFailed);
		return Keep(Action);
	}

	UFlockRestorePurchasesAction* Restore(UFlockRestorePurchasesAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::RestoreSucceeded);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::RestoreFailed);
		return Keep(Action);
	}

	UFlockCheckStoreSetupAction* Setup(UFlockCheckStoreSetupAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::SetupSucceeded);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::SetupFailed);
		return Keep(Action);
	}

	UFlockStartCheckoutAction* Checkout(UFlockStartCheckoutAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::CheckoutSucceeded);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::CheckoutFailed);
		return Keep(Action);
	}

	UFlockGetPlayerPurchaseAction* Read(UFlockGetPlayerPurchaseAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::PurchaseRead);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::PurchaseReadFailed);
		return Keep(Action);
	}

	UFlockGetPlayerPurchasesAction* List(UFlockGetPlayerPurchasesAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::PageRead);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::PageReadFailed);
		return Keep(Action);
	}

	UFlockRedeemStoreReceiptAction* Redeem(UFlockRedeemStoreReceiptAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::Bought);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::NotBought);
		return Keep(Action);
	}

	UFlockPurchaseAction* Buy(UFlockPurchaseAction* Action)
	{
		Action->OnSuccess.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::Bought);
		Action->OnFailure.AddDynamic(Listener.Get(), &UFlockStoreNodeTestListener::NotBought);
		return Keep(Action);
	}
};
