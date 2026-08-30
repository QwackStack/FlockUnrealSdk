// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockShopModels.h"

#include "Http/FlockJsonUtils.h"

namespace
{
	/**
	 * Reads a `granted` / `rewards` array into typed entries.
	 *
	 * An **absent or null key yields an empty array, never a failure**: the server omits it for anything
	 * that grants nothing, so treating a missing key as malformed would fail the common case. Each element
	 * goes through WireObjectToStruct so the snake->Pascal transform is applied in the one place that owns
	 * it, and a reward that omits `type` keeps the struct's own "currency" default.
	 */
	void ReadRewards(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, TArray<FFlockShopItemReward>& OutRewards)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Object->TryGetArrayField(Field, Entries) || Entries == nullptr)
		{
			return;
		}
		OutRewards.Reserve(Entries->Num());
		for (const TSharedPtr<FJsonValue>& Element : *Entries)
		{
			// Guard the type before AsObject(): a non-object would otherwise land as an empty reward,
			// which reads as a zero-amount grant rather than as the malformed entry it is.
			if (!Element.IsValid() || Element->Type != EJson::Object)
			{
				continue;
			}
			FFlockShopItemReward Reward;
			FString ElementError;
			if (FFlockJsonUtils::WireObjectToStruct(Element->AsObject().ToSharedRef(), Reward, ElementError))
			{
				OutRewards.Add(Reward);
			}
		}
	}

	/** Reads a nullable nested model. An absent or null field leaves OutStruct default-constructed. */
	template <typename T>
	void ReadOptional(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, T& OutStruct)
	{
		const TSharedPtr<FJsonObject>* Nested = nullptr;
		if (!Object->TryGetObjectField(Field, Nested) || Nested == nullptr || !Nested->IsValid())
		{
			return;
		}
		FString NestedError;
		FFlockJsonUtils::WireObjectToStruct(Nested->ToSharedRef(), OutStruct, NestedError);
	}
}

FFlockShopData FFlockShopData::FromWire(const TSharedPtr<FJsonObject>& Object)
{
	FFlockShopData Result;
	if (!Object.IsValid())
	{
		return Result;
	}

	Object->TryGetStringField(TEXT("web_shop_url"), Result.WebShopUrl);
	Object->TryGetStringField(TEXT("pwa_shop_url"), Result.PwaShopUrl);

	// `stats` is an open dict — kept verbatim inside the handle (author keys never case-transformed).
	Result.Stats = FFlockJsonData::FromJson(Object->TryGetField(TEXT("stats")));
	return Result;
}

bool FFlockShopItem::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockShopItem& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("name"), OutStruct.Name);
	Object->TryGetStringField(TEXT("status"), OutStruct.Status);
	Object->TryGetStringField(TEXT("shop_id"), OutStruct.ShopId);
	Object->TryGetStringField(TEXT("patch_id"), OutStruct.PatchId);
	Object->TryGetNumberField(TEXT("price"), OutStruct.Price);
	Object->TryGetStringField(TEXT("currency"), OutStruct.Currency);
	Object->TryGetStringField(TEXT("type"), OutStruct.Type);
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	ReadRewards(Object, TEXT("rewards"), OutStruct.Rewards);

	// `data` is an open dict — kept verbatim inside the handle rather than routed through the wire transform.
	OutStruct.Data = FFlockJsonData::FromJson(Object->TryGetField(TEXT("data")));
	return true;
}

bool FFlockPurchaseResult::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPurchaseResult& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("purchase_id"), OutStruct.PurchaseId);
	Object->TryGetStringField(TEXT("item_type"), OutStruct.ItemType);

	// Both nested models are nullable on the wire: a currency pack holds no inventory row, and a purchase
	// that moved no currency reports no wallet. Absent leaves the member default-constructed, so callers
	// test Id for emptiness rather than being handed a plausible-looking empty object they cannot detect.
	ReadOptional(Object, TEXT("inventory"), OutStruct.Inventory);
	ReadOptional(Object, TEXT("wallet"), OutStruct.Wallet);
	ReadRewards(Object, TEXT("granted"), OutStruct.Granted);
	return true;
}

bool FFlockConsumeResult::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockConsumeResult& OutStruct, FString& OutError)
{
	// `inventory` is required here, unlike a purchase — consuming acts on a row that already exists — but
	// it is still read through the same optional path: a server that broke that promise should surface as
	// an empty row the caller can test, not as a parse failure that hides the grant it reported alongside.
	ReadOptional(Object, TEXT("inventory"), OutStruct.Inventory);
	ReadOptional(Object, TEXT("wallet"), OutStruct.Wallet);
	ReadRewards(Object, TEXT("granted"), OutStruct.Granted);
	return true;
}

bool FFlockShop::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockShop& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("id"), OutStruct.Id);
	Object->TryGetStringField(TEXT("name"), OutStruct.Name);
	Object->TryGetStringField(TEXT("status"), OutStruct.Status);
	Object->TryGetStringField(TEXT("game_id"), OutStruct.GameId);
	Object->TryGetStringField(TEXT("game_version_id"), OutStruct.GameVersionId);
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	const TSharedPtr<FJsonObject>* DataObject = nullptr;
	if (Object->TryGetObjectField(TEXT("data"), DataObject) && DataObject != nullptr)
	{
		OutStruct.Data = FFlockShopData::FromWire(*DataObject);
	}

	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (Object->TryGetArrayField(TEXT("shop_items"), Items))
	{
		for (const TSharedPtr<FJsonValue>& Element : *Items)
		{
			// Guard the type before AsObject(): a non-object would otherwise deserialize as an empty item.
			if (!Element.IsValid() || Element->Type != EJson::Object)
			{
				continue;
			}
			FFlockShopItem Item;
			if (FFlockShopItem::FromWireObject(Element->AsObject().ToSharedRef(), Item, OutError))
			{
				OutStruct.ShopItems.Add(Item);
			}
		}
	}
	return true;
}
