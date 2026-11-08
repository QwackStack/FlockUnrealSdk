// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockJsonData.h"
#include "Models/FlockPlayerModels.h"
#include "FlockShopModels.generated.h"

/**
 * The reward kinds the server sends, as **string constants rather than a UENUM**.
 *
 * Rewards are stored server-side as typed entries precisely so new kinds can ship without a schema
 * migration, which makes this an open set the server owns. An enum would have to answer a value added
 * later either by failing the parse or by silently defaulting to the first enumerator — a client-side
 * break, or a wrong reward shown to a player, for a change that was designed to be additive. The same
 * call as FFlockNotification's Type/Severity, and the opposite of the leaderboard enums, where the spec
 * declares a closed set. The spec decides, consistently.
 */
namespace FlockShopItemRewardTypes
{
	inline constexpr const TCHAR* Currency = TEXT("currency");
}

/**
 * What kind of item a shop item is (FFlockShopItem::Type). An open set the server can widen, so compare against these,
 * never a literal; a kind added later still reads, through Type.
 */
namespace FlockShopItemTypes
{
	/** Lands in the player's inventory. In a store, set it up as a one-time (non-consumable) product. */
	inline constexpr const TCHAR* Standard = TEXT("standard");
	/** Pays its rewards straight into the wallet. In a store, set it up as a consumable product. */
	inline constexpr const TCHAR* CurrencyPack = TEXT("currency_pack");
	/** Lands in the inventory and grants its rewards when consumed. In a store, set it up as a consumable product. */
	inline constexpr const TCHAR* Consumable = TEXT("consumable");
}

/**
 * The stores and web payment gateways Flock takes real money through, as the server names them. Only the stores
 * (AppleAppStore, GooglePlay) are keys of FFlockShopItem::StoreProductIds: a web checkout needs no product id. An open set
 * the server can widen, so a store added later is kept, never dropped.
 */
namespace FlockPaymentProviders
{
	inline constexpr const TCHAR* AppleAppStore = TEXT("apple_app_store");
	inline constexpr const TCHAR* GooglePlay = TEXT("google_play");
	inline constexpr const TCHAR* Stripe = TEXT("stripe");
	inline constexpr const TCHAR* Tap = TEXT("tap");
	inline constexpr const TCHAR* Moyasar = TEXT("moyasar");
}

/**
 * Where a purchase has got to. A wallet purchase is born paid; the others belong to real-money checkouts, whose result comes
 * later. An open set the server can widen, so compare against these, never a literal.
 */
namespace FlockPurchaseStatuses
{
	inline constexpr const TCHAR* Pending = TEXT("pending");
	inline constexpr const TCHAR* Paid = TEXT("paid");
	inline constexpr const TCHAR* Failed = TEXT("failed");
	inline constexpr const TCHAR* Expired = TEXT("expired");
	inline constexpr const TCHAR* Refunded = TEXT("refunded");
}

/**
 * One thing a reward-bearing shop item grants (OpenAPI ShopItemRewardSchema).
 *
 * Code is a currency code from the game version's currency config — the same namespace an item's
 * Currency draws from. All scalar, so it takes the reflection wire path; Type carries the wire default
 * so an entry that omits it still reads as a currency grant rather than an empty string.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockShopItemReward
{
	GENERATED_BODY()

	/** Compare against FlockShopItemRewardTypes, never a literal — the server may add kinds. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Type = FlockShopItemRewardTypes::Currency;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Code;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Amount = 0;

	/** True for a currency grant — the only kind that exists today, and the reason Type is not an enum. Letter case counts. */
	bool IsCurrency() const { return Type.Equals(FlockShopItemRewardTypes::Currency, ESearchCase::CaseSensitive); }
};

/**
 * A shop's `data` object (OpenAPI ShopDataSchema). The two URL members are typed; `stats` is an open
 * dict on the wire (Dictionary<string, object> upstream), which a USTRUCT cannot hold — it is exposed as
 * an FFlockJsonData handle, read with typed dotted-path getters (C++) or UFlockJsonDataLibrary (Blueprint).
 * Not routed through the reflection wire path (its parent supplies a custom parse), so it needs no
 * FromWireObject of its own; FFlockJsonData round-trips a snapshot by reflection.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockShopData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString WebShopUrl;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PwaShopUrl;

	/** The free-form `stats` object (empty when absent). Read with TryGet* / UFlockJsonDataLibrary. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockJsonData Stats;

	/** Builds the data block from a wire `data` object (null/absent → an empty block). */
	static FFlockShopData FromWire(const TSharedPtr<FJsonObject>& Object);
};

/**
 * A shop item (OpenAPI ShopItemSchema). Its `data` is an open dict on the wire, exposed as an
 * FFlockJsonData handle — so the model declares its own FromWireObject (picked up by FFlockJsonUtils)
 * instead of the reflection path, which would rewrite the author-supplied `data` keys through the
 * snake->Pascal transform. Every other member is a regular scalar.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockShopItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Status;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ShopId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PatchId;

	/** The price in Currency; for a real-money item, in the currency's smallest unit (199 is USD 1.99). */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Price = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Currency;

	/**
	 * True when Currency is one the game's currency config lists as real money: the item is paid for through a store or a
	 * web checkout, never from the wallet: Purchase refuses it with ShopRealCurrencyNeedsCheckout, and a store's purchase of
	 * it is redeemed with FFlockShopProvider::RedeemStoreReceipt. The server's own
	 * answer, sent with the catalog by every shop read: false when the server does not send it (an older server), and never
	 * guessed from the currency's code.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	bool IsRealMoney = false;

	/** What kind of item this is, one of FlockShopItemTypes: it drives whether buying grants rewards or lands in inventory. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Type;

	/** What this item grants when bought or consumed. Empty for a plain inventory item, never null. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItemReward> Rewards;

	/**
	 * This item's product id in each store, keyed by FlockPaymentProviders name, exactly as set in the Flock dashboard; empty
	 * when none is set. A store the SDK does not know yet is kept. A phone shows the player the store's own price for the
	 * product, in their currency and with their taxes, not Price. Look an id up with FindStoreProductId, which matches the
	 * store's name with its letter case (a map's own Find ignores case).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TMap<FString, FString> StoreProductIds;

	/** The free-form `data` object (empty when absent). Read with TryGet* / UFlockJsonDataLibrary. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockJsonData Data;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/**
	 * This item's product id in Store (one of FlockPaymentProviders), matching the store's name with its letter case. False,
	 * with OutProductId emptied, when the item has none there.
	 */
	bool FindStoreProductId(const FString& Store, FString& OutProductId) const;

	/** Builds the item from a wire object, capturing `data` and each store's product id verbatim. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockShopItem& OutStruct, FString& OutError);
};

/**
 * A shop (OpenAPI ShopSchema): its data block, its items, and the usual ids/timestamps. Declares a
 * custom FromWireObject because its `data.stats` and each item's `data` are free-form (see above); it
 * parses those and delegates each `shop_items` element to FFlockShopItem's parse.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockShop
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Status;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString GameVersionId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockShopData Data;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItem> ShopItems;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/** Builds the shop from a wire object, parsing its data block and items. */
	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockShop& OutStruct, FString& OutError);
};

/**
 * A player's owned entry (OpenAPI PlayerInventorySchema / PlayerInventoryDetailSchema).
 *
 * Scalars plus a reward list whose members are themselves scalar, so it takes the reflection wire path —
 * the key transform recurses into arrays, so no custom parse is needed.
 *
 * `Rewards` is the one member the plain listing route does not send: the backend keeps the reward
 * snapshot on a separate detail schema so the frozen list shape stays as it was, and only the purchase
 * and consume routes carry it. Absent means an empty array here, which is what a caller should expect —
 * never a reason to null-check before iterating.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerInventory
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PlayerId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ShopItemId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Status;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UsedAt;

	/** The reward snapshot taken when this row was bought. Empty on the plain listing route. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItemReward> Rewards;
};

/**
 * What one purchase produced (OpenAPI PurchaseResultSchema): the inventory row, whatever the purchase
 * granted immediately, and the wallet after the debit and any grant.
 *
 * **Inventory is genuinely optional.** A currency pack hands its contents over on the spot and so holds
 * nothing — the wire sends null, which arrives here as a default-constructed row. Test `Inventory.Id`
 * for emptiness rather than assuming a row is always there. `Wallet` is likewise absent unless the
 * purchase moved currency, and carrying it is what lets a shop show the new balance without a second
 * call.
 *
 * Declares a custom FromWireObject: `wallet` is an FFlockPlayerData, whose own parse flattens its `data`
 * tree, and the reflection path would not reach it.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPurchaseResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PurchaseId;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ItemType;

	/** Where the purchase has got to: one of FlockPurchaseStatuses. Paid when the server leaves it out, as its schema does. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Status = FlockPurchaseStatuses::Paid;

	/** The row the purchase created. Empty Id when the item granted its contents outright. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockPlayerInventory Inventory;

	/** What the purchase granted immediately. Empty for a plain inventory item, never null. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItemReward> Granted;

	/** Balances after the debit and any grant. Empty Id when the purchase moved no currency. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockPlayerData Wallet;

	/**
	 * True when the purchase created a row the player now owns.
	 *
	 * Ask this rather than testing `Inventory.Id` yourself: an empty row is a **meaningful** answer here
	 * (the item handed its contents over outright), not a malformed response, and nothing about a bare
	 * emptiness check says so at the call site. Same shape as FFlockPlayerBan::IsBanned().
	 */
	bool HasInventoryRow() const { return !Inventory.Id.IsEmpty(); }

	/** True when the server reported the wallet — it does so only when currency actually moved. */
	bool HasWallet() const { return !Wallet.Id.IsEmpty(); }

	/** True when the purchase is paid, so what it grants is the player's. A pending one has handed nothing over yet. */
	bool IsPaid() const { return Status.Equals(FlockPurchaseStatuses::Paid, ESearchCase::CaseSensitive); }

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPurchaseResult& OutStruct, FString& OutError);
};

/**
 * The result of consuming an owned inventory entry (OpenAPI ConsumeResultSchema): the updated row, what
 * consuming it granted, and the wallet afterwards.
 *
 * Unlike a purchase the row is always present — consuming acts on one that already exists. `Wallet` is
 * null when the entry granted nothing. Custom parse for the same reason as above.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockConsumeResult
{
	GENERATED_BODY()

	/** The consumed row, with its used-at stamp set. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockPlayerInventory Inventory;

	/** What consuming it granted. Empty when it granted nothing, never null. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItemReward> Granted;

	/** Balances after the grant. Empty Id when nothing was granted. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockPlayerData Wallet;

	/** True when consuming the entry moved currency and the server reported the new balances. */
	bool HasWallet() const { return !Wallet.Id.IsEmpty(); }

	static bool FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockConsumeResult& OutStruct, FString& OutError);
};

/**
 * One page of shops ({items, total, page, limit}). A concrete USTRUCT rather than the template
 * TFlockPage<T>: it must round-trip the offline snapshot by reflection and be a Blueprint type, neither
 * of which a template can be. The provider copies the client's TFlockPage<FFlockShop> into this.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockShopPage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShop> Items;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Total = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Page = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Limit = 0;
};

/** One page of inventory entries ({items, total, page, limit}). Concrete for the same reasons as above. */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockPlayerInventoryPage
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockPlayerInventory> Items;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Total = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Page = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Limit = 0;
};

/** Body for `POST shop/transaction`. Both members are required by the backend. */
USTRUCT()
struct FLOCK_API FFlockShopTransactionRequest
{
	GENERATED_BODY()

	UPROPERTY() FString ShopItemId;
	UPROPERTY() FString PlayerId;
};

/**
 * A purchase a store made, as Flock redeems it: the App Store's transaction id, or Google Play's product id and purchase
 * token. Make one with FromAppleAppStore or FromGooglePlay, and hand it to FFlockShopProvider::RedeemStoreReceipt.
 * Each id is kept exactly as given: one with spaces around it is refused when redeemed, never trimmed.
 */
USTRUCT(BlueprintType)
struct FLOCK_API FFlockStoreReceipt
{
	GENERATED_BODY()

	/** The store that made the purchase: FlockPaymentProviders::AppleAppStore or FlockPaymentProviders::GooglePlay. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Store;

	/** The App Store's transaction id (StoreKit's transaction id); empty for Google Play. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString TransactionId;

	/** The Google Play product id the player bought; empty for the App Store. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString ProductId;

	/** The Google Play purchase token; empty for the App Store. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString PurchaseToken;

	/** An App Store purchase, by its transaction id, exactly as StoreKit gave it. */
	static FFlockStoreReceipt FromAppleAppStore(const FString& InTransactionId)
	{
		FFlockStoreReceipt Receipt;
		Receipt.Store = FlockPaymentProviders::AppleAppStore;
		Receipt.TransactionId = InTransactionId;
		return Receipt;
	}

	/** A Google Play purchase, by its product id and purchase token, exactly as Google Play gave them. */
	static FFlockStoreReceipt FromGooglePlay(const FString& InProductId, const FString& InPurchaseToken)
	{
		FFlockStoreReceipt Receipt;
		Receipt.Store = FlockPaymentProviders::GooglePlay;
		Receipt.ProductId = InProductId;
		Receipt.PurchaseToken = InPurchaseToken;
		return Receipt;
	}
};
