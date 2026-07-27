// Copyright 2022, Qwacks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Models/FlockJsonData.h"
#include "FlockShopModels.generated.h"

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
 * A player's owned entry (OpenAPI PlayerInventorySchema). All scalar, so it takes the reflection wire
 * path — no custom parse.
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
