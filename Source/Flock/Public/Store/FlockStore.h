// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "FlockEventModels.h"
#include "Models/FlockShopModels.h"
#include "FlockStore.generated.h"

class UFlockSubsystem;
class UFlockStore;

/**
 * Why a store did not take a payment (FFlockError::StoreFailureReason, on an error of type Store). An open set: a reason
 * added later reads as Unknown on an older SDK.
 */
namespace FlockStoreFailureReasons
{
	/** The player closed the store's purchase dialog. Nothing was charged. */
	inline constexpr const TCHAR* Cancelled = TEXT("cancelled");
	/** The store, or a web checkout's payment gateway, declined the player's payment. Nothing was charged. */
	inline constexpr const TCHAR* PaymentDeclined = TEXT("payment_declined");
	/** This store account already owns the one-time item. */
	inline constexpr const TCHAR* AlreadyOwned = TEXT("already_owned");
	/** An earlier purchase of the product is still waiting for its payment. */
	inline constexpr const TCHAR* AlreadyPending = TEXT("already_pending");
	/** The store has no active product with the item's product id. */
	inline constexpr const TCHAR* ProductUnavailable = TEXT("product_unavailable");
	/** The store could not be reached, or purchases are not allowed on this device. */
	inline constexpr const TCHAR* StoreUnavailable = TEXT("store_unavailable");
	/** The player is not signed in to the store on this device. */
	inline constexpr const TCHAR* NotSignedInToTheStore = TEXT("not_signed_in_to_the_store");
	/** A web checkout closed unpaid: not paid within the hour it stays open, or cancelled by the gateway. Nothing was charged. */
	inline constexpr const TCHAR* Expired = TEXT("expired");
	/** Any other reason; the error's Message carries the store's own words. */
	inline constexpr const TCHAR* Unknown = TEXT("unknown");
}

/** How UFlockStore::Connect pays real money. Every setting can be left at its default. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStoreOptions
{
	GENERATED_BODY()

	/**
	 * What to pay through: empty for this platform's own way (Google Play on Android, the App Store on iOS, a web checkout
	 * through the game's one active payment gateway on Windows, Mac and Linux), or one of FlockPaymentProviders: a store
	 * (GooglePlay, AppleAppStore), or a payment gateway (Stripe, Tap, Moyasar) when the game has several active. A phone is
	 * never sent to a web page: a gateway named there is refused, as the phone stores' rules forbid paying for digital goods
	 * elsewhere.
	 */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Flock")
	FString PaymentProvider;

	/** On a web checkout, where the player's browser lands after paying; empty leaves it to the server (its own page). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Flock")
	FString ReturnUrl;

	/** On a web checkout, where the player's browser lands after giving up; empty uses ReturnUrl, or the server's own page. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Flock")
	FString CancelUrl;

	/** On a web checkout, about how often a checkout waiting for its payment is read, in seconds (each wait varies by a quarter either way); 3 when zero, less, or not a number. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Flock")
	float CheckEverySeconds = 0.f;

	/**
	 * On a web checkout, opens the payment gateway's page; empty opens it in the player's default browser
	 * (FPlatformProcess::LaunchURL). Set it to show the address another way, such as a Pay now button or a QR code.
	 */
	TFunction<void(const FString& PaymentUrl)> OpenPaymentPage;
};

/** A real-money item's price as the store shows it to this player: in their currency and with their taxes. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStorePrice
{
	GENERATED_BODY()

	/** False when the store did not find the item's product (check the product id and that it is active in the store). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool bFoundInTheStore = false;

	/** The price written out, such as "SAR 4.59" or "$1.09": show this one. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Text;

	/** The price in CurrencyCode's smallest unit, as the store reports it (459 is SAR 4.59). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int64 AmountInSmallestUnit = 0;

	/** The currency's ISO 4217 code, such as SAR or USD. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CurrencyCode;
};

/** A store purchase the player paid for that Flock did not deliver. It stays unfinished in the store, which hands it back at the next launch to be sent again. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockUndeliveredPurchase
{
	GENERATED_BODY()

	/** False when the purchase could not be matched to one shop item (two items sell its product, or the item is gone). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool bHasItem = false;

	/** The shop item, when bHasItem. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockShopItem Item;

	/** The store's product id. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ProductId;

	/** Why Flock did not deliver it: its ErrorCode and Hint say what to do. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockError Error;
};

/** What UFlockStore::Restore found. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStoreRestore
{
	GENERATED_BODY()

	/** The shop items Flock holds for the signed-in player from this store account's purchases; one delivered before grants nothing more. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItem> Restored;

	/** Purchases Flock did not deliver, such as one bought on another Flock account. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockUndeliveredPurchase> NotDelivered;
};

/** What UFlockStore::CheckSetup found, without buying anything. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStoreSetupCheck
{
	GENERATED_BODY()

	/** Real-money items with a product id for this store that the store did not find: check the id and that the product is active in the store. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItem> NotFoundInTheStore;

	/** Real-money items with no product id for this store: they cannot be bought here. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItem> MissingProductId;

	/**
	 * Whether Flock's server could ask the store about a purchase with the game's keys. False with ServerProblem set when it
	 * could not; false with no problem when there was no item to ask about.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool bServerCanCheckPurchases = false;

	/** True when ServerProblem holds why Flock's server could not ask the store. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool bHasServerProblem = false;

	/**
	 * On a web checkout, what could not be checked and how to check it by hand: Flock's server says whether a payment gateway
	 * is connected only as a checkout opens. Empty on a store.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString NotChecked;

	/** Why Flock's server could not ask the store (its ErrorCode and Hint say what to fix), when bHasServerProblem. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockError ServerProblem;

	/**
	 * True when nothing above needs fixing. On a web checkout, where the gateway cannot be checked without opening a checkout,
	 * true when nothing was found to fix: NotChecked still says how to check the gateway by hand.
	 */
	bool IsReady() const
	{
		const bool bCheckedWhatItCould = bServerCanCheckPurchases || !NotChecked.IsEmpty();
		return bCheckedWhatItCould && NotFoundInTheStore.Num() == 0 && MissingProductId.Num() == 0;
	}
};

/** A store product as Flock sells it: consumable (a currency pack or a consumable), or one-time (a standard item). */
struct FFlockStoreProduct
{
	FString ProductId;
	bool bConsumable = false;
};

/** A purchase the store charged for (or is waiting to be paid for): its product and the store's own id for it. */
struct FFlockStoreOrder
{
	FString ProductId;
	/** Google Play's purchase token, or the App Store's transaction id; empty while a payment is pending. */
	FString TransactionId;
	/**
	 * True when this is the store's answer to Buy (the purchase dialog Flock opened); false for a purchase the store hands
	 * back any other way (one unfinished at an earlier launch, a payment completed later). Only the dialog's own answer
	 * completes the purchase call that opened it.
	 */
	bool bAnswersThePurchaseDialog = false;
};

/** A purchase the store did not take: the product, why (one of FlockStoreFailureReasons), and the store's own words. */
struct FFlockStoreFailure
{
	FString ProductId;
	FString Reason;
	FString Text;
};

/**
 * What UFlockStore needs of a platform's store. Flock's store module supplies one through the engine's online
 * subsystem and registers it with UFlockStore::SetStoreLibrary; a game with its own store code can supply its own. Every call
 * is made on the game thread, and each callback must be raised on it.
 */
class FLOCK_API IFlockStoreConnection
{
public:
	/** What the store tells UFlockStore outside a call. */
	struct FListener
	{
		/**
		 * The store charged for a purchase: one bought now, or one an earlier launch left unfinished. The answer to Buy sets
		 * FFlockStoreOrder::bAnswersThePurchaseDialog; without it the purchase is delivered, but the purchase call that opened
		 * the dialog never completes and no other store purchase can start.
		 */
		TFunction<void(const FFlockStoreOrder&)> OnPaid;
		/** The store is waiting for the payment (a pending payment or a parent's approval); only ever Buy's answer. */
		TFunction<void(const FFlockStoreOrder&)> OnDeferred;
		/** The store did not take a purchase started with Buy; only ever Buy's answer. */
		TFunction<void(const FFlockStoreFailure&)> OnFailed;
	};

	virtual ~IFlockStoreConnection() = default;

	/** The store this connection reaches: FlockPaymentProviders::GooglePlay or FlockPaymentProviders::AppleAppStore. */
	virtual FString GetPaymentProvider() const = 0;

	/** Starts telling Listener about orders. Called before Connect, so purchases handed back while connecting are heard. */
	virtual void Listen(FListener Listener) = 0;

	/** Connects to the store. OnDone gets an error of type None once connected, or one saying why it could not and how to fix it. */
	virtual void Connect(TFunction<void(const FFlockError& Error)> OnDone) = 0;

	/** Asks the store for the products' details (their prices). OnDone runs once the store answered, whether it found them or not. */
	virtual void RegisterProducts(const TArray<FFlockStoreProduct>& Products, TFunction<void()> OnDone) = 0;

	/** Opens the store's purchase dialog for a registered product; the answer comes through the listener. */
	virtual void Buy(const FString& ProductId) = 0;

	/** Finishes a purchase Flock delivered: a consumable product is consumed, a one-time product acknowledged. */
	virtual void Finish(const FFlockStoreOrder& Order) = 0;

	/** Asks the store for purchases still unfinished; each comes through the listener's OnPaid. */
	virtual void FetchUnfinishedPurchases() = 0;

	/** The store's own price of a registered product; bFoundInTheStore false when the store did not find it. */
	virtual FFlockStorePrice PriceOf(const FString& ProductId) const = 0;

	/** Purchases this store account owns and has finished, after asking the store to restore them. */
	virtual void FetchOwnedPurchases(TFunction<void(TFlockResult<TArray<FFlockStoreOrder>>)> OnDone) = 0;

	/** Stops telling the listener anything and ends the connection's use of the store. */
	virtual void StopListening() = 0;
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FFlockOnStorePurchaseCompleted, const FFlockShopItem& /*Item*/, const FFlockPurchaseResult& /*Result*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FFlockOnStorePurchaseNotDelivered, const FFlockUndeliveredPurchase& /*Purchase*/);

/**
 * The game's way to pay real money: connect it once, and FFlockShopProvider::Purchase buys real-money items through it. The
 * store charges the player, Flock checks the purchase with the store and delivers the item, and only then is the store's
 * purchase finished; a purchase Flock did not deliver stays unfinished, so the store hands it back at the next launch, where it
 * is sent again once a player is signed in. The platform's store is reached through a store library, which Flock's store
 * module registers; without one a store is refused, never replaced by a web page, as a phone store's rules forbid paying
 * for digital goods elsewhere. On Windows, Mac and Linux the way to pay is a web checkout: the purchase opens a checkout
 * with the game's payment gateway (Stripe, Tap or Moyasar), opens its payment page in the player's browser and waits until
 * Flock's server reports it paid, failed or expired; Flock's server delivers the item itself.
 *
 * One connection at a time: the subsystem holds it (UFlockSubsystem::GetStore) until Disconnect or the SDK shuts down. Game
 * thread only.
 */
UCLASS(BlueprintType)
class FLOCK_API UFlockStore : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Connects the game's way to pay real money: Options.PaymentProvider, or this platform's own when it is empty (the store on
	 * a phone, a web checkout on Windows, Mac and Linux). Reads the game's shops. A store also registers each item that has a
	 * product id for it (a currency pack or consumable as a consumable product, a standard item as a one-time product) and from
	 * then on sends Flock the store's unfinished purchases whenever a player is signed in; a web checkout reads the signed-in
	 * player's checkouts still open, at connect and at every sign-in. Refused when no store library is registered (Flock's
	 * store module not loaded), for a payment gateway named on a phone, and while another connection is connected (Disconnect
	 * it first). A web checkout in a build whose engine online service is Steam logs one warning: Steam's rules allow in-game
	 * purchases only through the Steam wallet. Completes with the connection, which the subsystem also holds.
	 */
	static void Connect(UFlockSubsystem* Flock, const FFlockStoreOptions& Options, TFunction<void(TFlockResult<UFlockStore*>)> OnComplete);

	/**
	 * Makes Factory the way UFlockStore reaches the platform's store. Flock's store module calls this as it starts; a
	 * game with its own store code may call it instead. Only connections made afterwards use it.
	 */
	static void SetStoreLibrary(TFunction<TSharedPtr<IFlockStoreConnection>()> Factory);

	/** Forgets the store library (Flock's store module shutting down). A connection already made keeps its own. */
	static void ClearStoreLibrary();

	/** True when a store library is registered, so a store can be connected. */
	static bool HasStoreLibrary();

	/** The store library registered, or null; a caller that replaces it for a while puts this one back afterwards. */
	static TFunction<TSharedPtr<IFlockStoreConnection>()> GetStoreLibrary();

	/**
	 * What this connection pays through: FlockPaymentProviders::GooglePlay or AppleAppStore, the payment gateway named in
	 * FFlockStoreOptions::PaymentProvider, or empty for a web checkout through the game's one active gateway.
	 */
	UFUNCTION(BlueprintPure, Category = "Flock|Store")
	FString GetPaymentProvider() const { return PaymentProvider; }

	/** True when purchases go through a web checkout (the gateway's payment page in the player's browser), false through a phone's store. */
	UFUNCTION(BlueprintPure, Category = "Flock|Store")
	bool IsWebCheckout() const { return Web.IsValid(); }

	/** True from a successful Connect until Disconnect or the SDK shutting down. */
	UFUNCTION(BlueprintPure, Category = "Flock|Store")
	bool IsConnected() const { return bReady && !bDisconnected; }

	/**
	 * A real-money item's price to show this player. On a phone, as the store shows it, in their currency and with their
	 * taxes, rather than its Price: bFoundInTheStore is false when the store did not find the item's product. On a web
	 * checkout, the item's own price, which is what the gateway charges. Fails (Validation) for an item not in the game's
	 * shops when the store connected, one with no product id for this store, and on a web checkout one priced in a game
	 * currency.
	 */
	TFlockResult<FFlockStorePrice> GetPrice(const FString& ShopItemId) const;

	/**
	 * Asks the store for the one-time purchases this store account owns and has Flock deliver each to the signed-in player;
	 * a purchase Flock delivered before grants nothing more. Purchases still unfinished, one whose send failed earlier in this
	 * launch included, are sent again and reported through OnPurchaseCompleted or OnPurchaseNotDelivered as usual. One made for
	 * another Flock account is listed in NotDelivered. Fails when no player is signed in, when the store cannot list the
	 * purchases, and (Cancelled) when the player signs out or changes while it runs, sending none of the rest. On a web
	 * checkout Flock already holds what the player bought: the player's checkouts still open are read again (each one paid is
	 * reported through OnPurchaseCompleted; one paid while nothing read it is delivered but not reported), and both lists are
	 * empty. Several restores may run at once there.
	 */
	void Restore(TFunction<void(TFlockResult<FFlockStoreRestore>)> OnComplete);

	/**
	 * Checks the store's setup without buying anything: which real-money items the store did not find or have no product id
	 * for this store, and whether Flock's server can ask the store about a purchase with the game's keys (it is asked about a
	 * made-up purchase, which the store answers as unknown when the keys work). Needs a signed-in player. On a web checkout
	 * nothing can be asked without opening a checkout, so NotChecked says how to check the gateway, and no checkout is opened.
	 */
	void CheckSetup(TFunction<void(TFlockResult<FFlockStoreSetupCheck>)> OnComplete);

	/**
	 * Stops listening to the store and to sign-ins; FFlockShopProvider::Purchase no longer buys real-money items through it,
	 * and a purchase still waiting ends Cancelled. A store purchase paid afterwards stays unfinished and is handed back to the
	 * next connection, and so does one Flock answers only after the disconnect: it is neither finished nor reported here, and
	 * the next connection's send of it is answered with what Flock delivered, then finished and reported.
	 */
	UFUNCTION(BlueprintCallable, Category = "Flock|Store")
	void Disconnect();

	/**
	 * Raised for every real-money purchase Flock delivers: one bought just now, one the store was waiting to be paid for, one
	 * left unfinished at an earlier launch, and one a restore sent again; on a web checkout, once for every checkout seen to be
	 * paid, whether its call still waited or it was found still unpaid at a sign-in (an item that can no longer be read comes
	 * with its Id alone). A checkout paid while nothing read it (after a sign-out, a disconnect, or with the game closed) is
	 * delivered by Flock but not reported: refresh the wallet and inventory after each sign-in.
	 * Refresh the wallet and inventory here. Blueprint binds UFlockEvents::OnStorePurchaseCompleted, raised right after this.
	 */
	FFlockOnStorePurchaseCompleted OnPurchaseCompleted;

	/** Raised for every store purchase the player paid for that Flock did not deliver; it stays unfinished in the store and is sent again at the next launch. Never raised on a web checkout, which Flock's server delivers itself. */
	FFlockOnStorePurchaseNotDelivered OnPurchaseNotDelivered;

	/**
	 * FFlockShopProvider::Purchase's real-money path: opens the store's purchase dialog for the item and, once the store has
	 * charged, has Flock deliver it before the store's purchase is finished. Fails Store with StoreFailureReason when the store
	 * did not take the payment; a payment the store waits for completes with Status pending, and OnPurchaseCompleted follows
	 * once paid. On a web checkout: reads the item, opens a checkout, opens its payment page and completes once it is paid,
	 * with PurchaseId, ItemType, Status and Granted (no Inventory or Wallet: read them after). A payment the gateway declines
	 * fails Store (PaymentDeclined); a page the player closes keeps the call waiting until the checkout expires after its hour
	 * (Expired); reads that stop on another failure end with bOutcomeUnknown, as it may still be paid. Never sent again after
	 * an answer that did not come, so it never opens two checkouts.
	 */
	void Purchase(const FString& ShopItemId, TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete);

	/** The shop item this connection read when it connected, or false. */
	bool FindItem(const FString& ShopItemId, FFlockShopItem& OutItem) const;

	/** This build's own store: Google Play on Android, the App Store on iOS, empty elsewhere. */
	static FString PaymentProviderOfThisPlatform();

	/** True on Windows, Mac and Linux: the platforms a web checkout is opened on. */
	static bool IsADesktop();

	/** True when the engine's default online service is Steam ([OnlineSubsystem] DefaultPlatformService in the engine settings). */
	static bool IsSteamTheDefaultService();

	/** Connects through Connection rather than the store library: for a game's own store code, and for tests. */
	static void ConnectThrough(UFlockSubsystem* Flock, const TSharedRef<IFlockStoreConnection>& Connection,
		TFunction<void(TFlockResult<UFlockStore*>)> OnComplete);

	virtual void BeginDestroy() override;

	/** Ids match letter for letter, as the stores' and the server's do (a map of strings ignores case). */
	template <typename ValueType>
	struct FExactIdKeyFuncs : TDefaultMapKeyFuncs<FString, ValueType, /*bInAllowDuplicateKeys*/ false>
	{
		static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
	};

private:
	template <typename ValueType>
	using TExactIdMap = TMap<FString, ValueType, FDefaultSetAllocator, FExactIdKeyFuncs<ValueType>>;

public:
	/** A set of ids that match letter for letter. */
	struct FExactIdSetKeyFuncs : DefaultKeyFuncs<FString>
	{
		static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
	};
	using FExactIdSet = TSet<FString, FExactIdSetKeyFuncs>;

private:
	struct FBuying
	{
		/** Which purchase call this is, so an answer for an earlier call of the same product finds nothing. */
		int32 Number = 0;
		FFlockShopItem Item;
		/** The sign-in the store's dialog was opened under; INDEX_NONE until then. */
		int32 SignIn = INDEX_NONE;
		TFunction<void(TFlockResult<FFlockPurchaseResult>)> OnComplete;
	};

	/** A restore under way: the store's owned purchases, what was found so far, and the sign-in it is for. */
	struct FRestoring
	{
		TArray<FFlockStoreOrder> Owned;
		FFlockStoreRestore Found;
		int32 SignIn = INDEX_NONE;
		TFunction<void(TFlockResult<FFlockStoreRestore>)> OnComplete;
		bool bEnded = false;

		/** Answers the restore's caller once; any later answer finds it ended. */
		void End(const TFlockResult<FFlockStoreRestore>& Result)
		{
			if (bEnded)
			{
				return;
			}
			bEnded = true;
			if (OnComplete)
			{
				OnComplete(Result);
			}
		}
	};

	/** A purchase the store's dialog answered after its call ended, kept for the player who opened the dialog. */
	struct FWaitingForBuyer
	{
		FString PlayerId;
		FFlockStoreOrder Order;
	};

	/** Ends purchase call Number of ProductId once, taking its callback out of Buying, so a late answer finds nothing. */
	void EndBuying(const FString& ProductId, int32 Number, const TFlockResult<FFlockPurchaseResult>& Result);
	/** Takes purchase call Number of ProductId out of Buying and hands back its callback (empty when it ended already). */
	TFunction<void(TFlockResult<FFlockPurchaseResult>)> TakeBuying(const FString& ProductId, int32 Number);
	void EndBuyingIfSignInEnded();
	void EndEveryBuying(const FFlockError& Error);

	void StartConnecting(TFunction<void(TFlockResult<UFlockStore*>)> OnComplete);
	/** A web checkout through Gateway (empty: the game's one active gateway): reads the shops, then the open checkouts. */
	static void ConnectWebCheckout(UFlockSubsystem* Flock, const FString& Gateway, const FFlockStoreOptions& Options,
		TFunction<void(TFlockResult<UFlockStore*>)> OnComplete);
	/** Refuses (OnComplete told) when Flock is not ready or a connection is already held; true when one may connect. */
	static bool MayConnect(UFlockSubsystem* Flock, TFunction<void(TFlockResult<UFlockStore*>)>& OnComplete);
	static FFlockStorePrice PriceOfTheItem(const FFlockShopItem& Item);
	void ReadCatalog(int32 Page, TSharedRef<TArray<FFlockShop>> Shops, TFunction<void(TFlockResult<TArray<FFlockShop>>)> OnDone);
	void LearnCatalog(const TArray<FFlockShop>& Shops, TArray<FFlockStoreProduct>& OutProducts);
	void RememberProducts(TArray<FFlockStoreProduct>& InOutProducts);
	void BeReady();
	void RestoreFrom(const TSharedRef<FRestoring>& Restoring, int32 Index);
	void RestoreOne(const TSharedRef<FRestoring>& Restoring, int32 Index, const FFlockStoreOrder& Order, const FFlockShopItem& Item);

	UFUNCTION()
	void HandleSignedIn(const FFlockAuthInfo& Info);
	UFUNCTION()
	void HandleSignInEnded();

	void HandlePaid(const FFlockStoreOrder& Order);
	void HandleDeferred(const FFlockStoreOrder& Order);
	void HandleFailed(const FFlockStoreFailure& Failure);
	void Hold(const FFlockStoreOrder& Order);
	/** The purchase call the store's dialog answers, or null: Order's dialog answer, for the product the open dialog is for. */
	const FBuying* CallAnsweredBy(const FString& ProductId, bool bAnswersThePurchaseDialog) const;
	/** Closes the store's dialog for ProductId (it answered); returns the player it was opened for, or empty. */
	FString CloseTheDialog(const FString& ProductId);
	void Send(const FFlockStoreOrder& Order, const FFlockShopItem& Item, int32 Number);
	void SendForAnItemNoLongerSold(const FFlockStoreOrder& Order, const FString& ShopItemId);
	void RaiseNotDelivered(const FFlockShopItem* Item, const FString& ProductId, const FFlockError& Error);
	/** Reports a delivered purchase: OnPurchaseCompleted, then Flock's events hub (UFlockEvents::OnStorePurchaseCompleted). */
	void ReportCompleted(const FFlockShopItem& Item, const FFlockPurchaseResult& Result);

	/** Refuses the calls of a connection no longer usable: disconnected (by the game, or by the SDK shutting down). */
	bool IsUsable(FFlockError& OutError) const;
	bool KnownItem(const FString& ShopItemId, FFlockShopItem& OutItem, FFlockError& OutError) const;
	bool ProductOf(const FString& ShopItemId, FFlockShopItem& OutItem, FString& OutProductId, FFlockError& OutError) const;
	FFlockStoreReceipt ReceiptOf(const FFlockStoreOrder& Order) const;
	FFlockError SoldTwice(const FString& ProductId, const FString& Then) const;
	int32 CurrentSignIn() const;
	bool IsSignedIn() const;
	FString CurrentPlayerId() const;

	/**
	 * The purchases sent, and being sent, during this run of the game, for this connection's game, store and saved files:
	 * kept for the whole run rather than the connection, so a connection made again never sends one a second time.
	 */
	FExactIdSet& SentThisLaunch() const;
	FExactIdSet& BeingSent() const;
	static FExactIdSet& BeingSentFor(const FString& LaunchKey);

	static bool IsAStore(const FString& PaymentProvider);
	static bool IsSoldHere(const FFlockShopItem& Item);
	static FString FixFor(const FString& Reason);

	TWeakObjectPtr<UFlockSubsystem> Flock;
	TSharedPtr<IFlockStoreConnection> Connection;
	int32 BuyingCount = 0;
	FString PaymentProvider;
	bool bReady = false;
	bool bDisconnected = false;

	TExactIdMap<FFlockShopItem> ItemsById;
	TExactIdMap<FFlockShopItem> ItemByProduct;
	FExactIdSet ProductsSoldTwice;
	TExactIdMap<FString> ShopOfItem;
	/** Products sold at an earlier launch for items the shops no longer list, by product id: their purchases are still sent. */
	TExactIdMap<FString> SoldBefore;
	TExactIdMap<FBuying> Buying;
	/** What SentThisLaunch and BeingSent are kept under: this connection's saved files, game and store. */
	FString LaunchKey;
	/** Purchases handed over before the shops were read or anyone signed in; sent once both are true. */
	TArray<FFlockStoreOrder> Waiting;
	/** The product whose purchase dialog the store has open, and the player it was opened for; one at a time. */
	FString DialogProduct;
	FString DialogBuyer;
	/** Dialog answers that came after their call ended, sent when the player who opened the dialog signs in again. */
	TArray<FWaitingForBuyer> WaitingForBuyer;
	/** The restore under way, if any: one at a time, and ended by a disconnect. */
	TSharedPtr<FRestoring> CurrentRestore;
	/** The web checkout this connection pays through; null on a store. */
	TSharedPtr<class FFlockWebCheckout> Web;
};
