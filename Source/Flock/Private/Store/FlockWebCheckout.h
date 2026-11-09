// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Models/FlockShopModels.h"
#include "Store/FlockStore.h"

class UFlockSubsystem;
class FFlockRepeatingCall;

/**
 * UFlockStore's way to pay away from a phone's store: a purchase opens a checkout with the game's payment gateway (Stripe,
 * Tap or Moyasar), opens its payment page in the player's browser and reads the purchase until it is paid, failed or expired.
 * Checkouts left open and still unpaid (an earlier launch, another device, a call whose sign-in ended) are found again at
 * each sign-in. Reads go through the one owner of repeating calls, on the core ticker, and end with the sign-in they were started under. A
 * checkout paid after its reads ended (a sign-out, a disconnect, the game closed) is delivered by Flock's server but not
 * reported, since only checkouts still pending are listed. A checkout is never opened twice for one call: StartCheckout is
 * never sent again after an answer that did not come. Game thread only.
 */
class FFlockWebCheckout : public TSharedFromThis<FFlockWebCheckout>
{
public:
	/** What UFlockStore::Connect was given for a web checkout, copied as connected. */
	struct FSettings
	{
		/** The gateway named, or empty for the game's one active gateway. */
		FString Gateway;
		FString ReturnUrl;
		FString CancelUrl;
		double CheckEverySeconds = 3.0;
		TFunction<void(const FString&)> OpenPaymentPage;
	};

	/** A checkout just opened may read as not found for this long: the server saves it just after answering. */
	static constexpr double NewCheckoutSavedWithinSeconds = 30.0;

	/** How often a checkout is read when FFlockStoreOptions::CheckEverySeconds is zero or less. */
	static constexpr double DefaultCheckEverySeconds = 3.0;

	/**
	 * @param InPaid UFlockStore's OnPurchaseCompleted, raised once for every paid checkout.
	 * @param InFindItem The shop item the connection read at connect, by id; false for one it did not read.
	 */
	FFlockWebCheckout(UFlockSubsystem* InFlock, FSettings InSettings,
		TFunction<void(const FFlockShopItem&, const FFlockPurchaseResult&)> InPaid, TFunction<bool(const FString&, FFlockShopItem&)> InFindItem);

	/** Stops every read still going, answering no one: the connection went without Dispose (garbage collected). */
	~FFlockWebCheckout();

	/**
	 * Opens a checkout for Item (a real-money item of the game's shops), opens its payment page and completes once the
	 * purchase is paid (the result), failed or expired (a Store error with its reason), or the wait ends (Cancelled).
	 */
	void Purchase(const FFlockShopItem& Item, TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete);

	/** Reads every page of the signed-in player's open checkouts and watches each until it ends. OnDone gets an error of type None once listed. */
	void FindOpenCheckouts(TFunction<void(const FFlockError&)> OnDone);

	/** A sign-in: ends what an earlier one started, then finds this one's open checkouts. */
	void HandleSignedIn();

	/** A sign-out or an expired sign-in: ends what it started. */
	void HandleSignInEnded();

	/** Stops every read; a purchase still waiting ends Cancelled. A checkout paid afterwards is delivered by the server but not reported; one still unpaid is read again by the next connection. */
	void Dispose();

	/** Stripe, Tap or Moyasar, letter for letter. */
	static bool IsWebGateway(const FString& PaymentProvider);

private:
	struct FWatch
	{
		FString PurchaseId;
		FString ShopItemId;
		bool bHasItem = false;
		FFlockShopItem Item;
		/** The sign-in the reads are made for. */
		int32 SignIn = INDEX_NONE;
		TSharedPtr<FFlockRepeatingCall> Call;
		/** The purchase call waiting for this checkout, if one is, and its hold on the item in Buying. */
		TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnDone;
		int32 BuyingNumber = INDEX_NONE;
		/** When this connection opened it, on the repeating calls' clock; below zero for one found in the player's list. */
		double OpenedAt = -1.0;
	};

	TSharedRef<FWatch> WatchCheckout(const FString& PurchaseId, const FString& ShopItemId, const FFlockShopItem* Item, bool bOpenedHere);
	void HandleRead(const TSharedRef<FWatch>& Watch, const FFlockPlayerPurchase& Purchase);
	void HandleStopped(const TSharedRef<FWatch>& Watch, const FFlockError& Failure);
	void HandlePaid(const TSharedRef<FWatch>& Watch, const FFlockPlayerPurchase& Purchase);
	void Forget(const TSharedRef<FWatch>& Watch);
	void EndWhatAnEndedSignInStarted();
	void FindOpenCheckoutsFrom(int32 Page, int32 SignIn, TFunction<void(const FFlockError&)> OnDone);
	bool OpenPaymentPage(const FString& PaymentUrl, FString& OutWhyNot) const;

	int32 CurrentSignIn() const;
	bool IsSignedIn() const;
	double Now() const;

	/** Takes the waiting call's answer out of Watch (empty when none waits) and releases its hold on the item. */
	TFunction<void(TFlockResult<FFlockPurchaseResult>)> Take(const TSharedRef<FWatch>& Watch);
	/** Releases purchase call Number's hold on the item; a later call's hold is left alone. */
	void ReleaseBuying(const FString& ShopItemId, int32 Number);
	static FString FixFor(const FString& Reason);

	TWeakObjectPtr<UFlockSubsystem> Flock;
	FSettings Settings;
	TFunction<void(const FFlockShopItem&, const FFlockPurchaseResult&)> Paid;
	TFunction<bool(const FString&, FFlockShopItem&)> FindItem;
	/** The checkout being read, by purchase id: one read each, whoever started it. */
	TMap<FString, TSharedRef<FWatch>, FDefaultSetAllocator, UFlockStore::FExactIdKeyFuncs<TSharedRef<FWatch>>> Watches;
	/** Purchases this sign-in saw end, so a list of open checkouts answered before one ended does not read and report it again. */
	UFlockStore::FExactIdSet Ended;
	/** The sign-in Ended belongs to. */
	int32 EndedSignIn = INDEX_NONE;
	/** Items a purchase call is buying now, with that call's number, so a second call for one is refused rather than opening two checkouts. */
	TMap<FString, int32, FDefaultSetAllocator, UFlockStore::FExactIdKeyFuncs<int32>> Buying;
	int32 BuyingCount = 0;
	bool bDisposed = false;
};
