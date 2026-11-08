// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Store/FlockStore.h"

class IOnlineSubsystem;
class FPurchaseReceipt;

/**
 * Flock's store reached through the engine's online subsystem: products and prices through IOnlineStoreV2, purchases through
 * IOnlinePurchase. The platform's default subsystem decides the store: Google Play (the GOOGLEPLAY subsystem) or the App Store
 * (IOS). Google Play is bought through only on UE 5.8 and later, whose Billing Library Google still takes.
 *
 * Purchases go unbound: the engine hashes the account id before Google sees it and never hands it to Apple, so no account id is
 * sent and the server marks each receipt unbound. A finished one-time item on Google Play is acknowledged, not consumed (the
 * engine consumes every product it is not told is a subscription). Game thread only, as the online subsystem is.
 */
class FLOCKPLATFORMSTORE_API FFlockOnlineStoreConnection
	: public IFlockStoreConnection
	, public TSharedFromThis<FFlockOnlineStoreConnection>
{
public:
	virtual ~FFlockOnlineStoreConnection() override;

	virtual FString GetPaymentProvider() const override;
	virtual void Listen(FListener InListener) override;
	virtual void Connect(TFunction<void(const FFlockError& Error)> OnDone) override;
	virtual void RegisterProducts(const TArray<FFlockStoreProduct>& Products, TFunction<void()> OnDone) override;
	virtual void Buy(const FString& ProductId) override;
	virtual void Finish(const FFlockStoreOrder& Order) override;
	virtual void FetchUnfinishedPurchases() override;
	virtual FFlockStorePrice PriceOf(const FString& ProductId) const override;
	virtual void FetchOwnedPurchases(TFunction<void(TFlockResult<TArray<FFlockStoreOrder>>)> OnDone) override;
	virtual void StopListening() override;

	/** The store a subsystem of this name reaches (FlockPaymentProviders), or empty when it is not a store. */
	static FString PaymentProviderOf(const FName& SubsystemName);

	/** Why a checkout failed, as one of FlockStoreFailureReasons, from the engine's error code for it. */
	static FString ReasonOf(const FString& EngineErrorCode);

	/** True when the engine's error code says the store is waiting for the payment, not that it failed. */
	static bool IsWaitingForPayment(const FString& EngineErrorCode);

	/**
	 * The receipt info FinalizePurchase is handed: on Google Play a one-time product says it is a subscription, so the engine
	 * acknowledges it rather than consuming it; everything else is finished as the engine finishes it.
	 */
	static FString FinishInfoFor(const FString& PaymentProvider, bool bConsumable);

	/**
	 * True when Google Play's receipt says the purchase is already acknowledged: a one-time item finished at an earlier launch,
	 * which the engine hands back at every launch. Its validation info is JSON whose `receiptData` is Google's purchase JSON,
	 * in Base64.
	 */
	static bool IsAcknowledged(const FString& ValidationInfo);

	/** Whether a receipt in this state is a purchase the store took the payment for. */
	static bool IsPaid(const FPurchaseReceipt& Receipt);

	/**
	 * Whether a receipt the store listed is a paid purchase still to be finished, so Flock is sent it: purchased, with its
	 * store id, and on Google Play with its validation info (none while the payment is pending) and not acknowledged.
	 */
	static bool IsUnfinished(const FPurchaseReceipt& Receipt, const FString& PaymentProvider);

	/** Whether a receipt a restore listed is a purchase this account owns and the store has finished. */
	static bool IsOwnedAndFinished(const FPurchaseReceipt& Receipt, const FString& PaymentProvider);

	/** The store's order for a receipt: its product and its store id. */
	static FFlockStoreOrder OrderOf(const FPurchaseReceipt& Receipt);

private:
	IOnlineSubsystem* Subsystem() const;
	void ReadReceipts(bool bRestore, TFunction<void(bool bListed, const FString& Error)> OnDone);
	void HandUnfinishedOver(const TArray<FPurchaseReceipt>& Receipts);

	FListener Listener;
	bool bConnected = false;
	bool bStopped = false;
	/** The store answers one receipt read at a time: a read asked for while one is out waits for it. */
	bool bReadingReceipts = false;
	TArray<TPair<bool, TFunction<void(bool, const FString&)>>> WaitingReads;
	TMap<FString, bool> ConsumableByProduct;
	FDelegateHandle UnexpectedPurchaseHandle;
};
