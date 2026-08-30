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

	/** True for a currency grant — the only kind that exists today, and the reason Type is not an enum. */
	bool IsCurrency() const { return Type == FlockShopItemRewardTypes::Currency; }
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

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	int32 Price = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Currency;

	/** What kind of item this is — it drives whether buying grants rewards or lands in inventory. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString Type;

	/** What this item grants when bought or consumed. Empty for a plain inventory item, never null. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	TArray<FFlockShopItemReward> Rewards;

	/** The free-form `data` object (empty when absent). Read with TryGet* / UFlockJsonDataLibrary. */
	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FFlockJsonData Data;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString CreatedAt;

	UPROPERTY(BlueprintReadOnly, Category = "Flock")
	FString UpdatedAt;

	/** Builds the item from a wire object, capturing `data` verbatim. */
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
