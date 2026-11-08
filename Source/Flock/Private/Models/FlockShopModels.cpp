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

	/**
	 * Reads `store_skus`, each store's product id. Kept verbatim, keys and values: a store compares product ids exactly, and a
	 * store the SDK does not know yet is kept. An absent or null map reads empty, and a member whose id is not a string is
	 * left out rather than failing the item.
	 */
	void ReadStoreProductIds(const TSharedRef<FJsonObject>& Object, TMap<FString, FString>& OutProductIds)
	{
		const TSharedPtr<FJsonObject>* Ids = nullptr;
		if (!Object->TryGetObjectField(TEXT("store_skus"), Ids) || Ids == nullptr || !Ids->IsValid())
		{
			return;
		}
		// `auto` and FString(*Entry.Key): UE 5.8 interns the key (see Misc/FlockEngineCompat.h).
		for (const auto& Entry : (*Ids)->Values)
		{
			if (Entry.Value.IsValid() && Entry.Value->Type == EJson::String)
			{
				OutProductIds.Add(FString(*Entry.Key), Entry.Value->AsString());
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

	// The server's word only: a JSON true. Absent (an older server), null or any other value reads false and never fails
	// the read; TryGetBoolField would also take the string "true", which the server never sends for this member.
	const TSharedPtr<FJsonValue> RealMoney = Object->TryGetField(TEXT("is_real_money"));
	OutStruct.IsRealMoney = RealMoney.IsValid() && RealMoney->Type == EJson::Boolean && RealMoney->AsBool();

	ReadRewards(Object, TEXT("rewards"), OutStruct.Rewards);
	ReadStoreProductIds(Object, OutStruct.StoreProductIds);

	// `data` is an open dict — kept verbatim inside the handle rather than routed through the wire transform.
	OutStruct.Data = FFlockJsonData::FromJson(Object->TryGetField(TEXT("data")));
	return true;
}

bool FFlockShopItem::FindStoreProductId(const FString& Store, FString& OutProductId) const
{
	OutProductId.Reset();
	for (const TPair<FString, FString>& Entry : StoreProductIds)
	{
		if (Entry.Key.Equals(Store, ESearchCase::CaseSensitive))
		{
			OutProductId = Entry.Value;
			return true;
		}
	}
	return false;
}

bool FFlockPurchaseResult::FromWireObject(const TSharedRef<FJsonObject>& Object, FFlockPurchaseResult& OutStruct, FString& OutError)
{
	Object->TryGetStringField(TEXT("purchase_id"), OutStruct.PurchaseId);
	// The member the answer exists for: without it the status below defaults to paid, and `{}` would read as a paid
	// purchase. Failing the decode makes every call that answers a purchase fail as an unreadable 2xx.
	if (OutStruct.PurchaseId.IsEmpty())
	{
		OutError = TEXT("The server's answer named no purchase, so whether it went through is unknown");
		return false;
	}
	Object->TryGetStringField(TEXT("item_type"), OutStruct.ItemType);
	// Absent keeps the member's default, paid, which is the schema's own default.
	Object->TryGetStringField(TEXT("status"), OutStruct.Status);

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
	// `inventory` is required here, unlike a purchase: consuming acts on a row that already exists. An answer naming no
	// entry is not Flock's answer about one, so whether it was used is unknown, and the decode fails the call.
	ReadOptional(Object, TEXT("inventory"), OutStruct.Inventory);
	if (OutStruct.Inventory.Id.IsEmpty())
	{
		OutError = TEXT("The server's answer named no inventory entry, so whether it was used is unknown");
		return false;
	}
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

// ─────────────────────────────── Wallet ───────────────────────────────

namespace
{
	void AddBalance(const FString& Code, const TSharedPtr<FJsonValue>& Value, TArray<FFlockCurrencyBalance>& OutBalances)
	{
		if (!Code.IsEmpty() && Value.IsValid() && Value->Type == EJson::Number)
		{
			FFlockCurrencyBalance Balance;
			Balance.Code = Code;
			Balance.Amount = static_cast<int64>(Value->AsNumber());
			OutBalances.Add(Balance);
		}
	}

	/** One wallet category as the server sends it: a field of type object whose value lists a field per currency code. */
	void ReadFieldList(const TArray<TSharedPtr<FJsonValue>>& Fields, const FString& Category, TArray<FFlockCurrencyBalance>& OutBalances)
	{
		for (const TSharedPtr<FJsonValue>& Element : Fields)
		{
			const TSharedPtr<FJsonObject> Field = FFlockJsonUtils::AsObjectOrNull(Element);
			FString Name;
			const TArray<TSharedPtr<FJsonValue>>* Codes = nullptr;
			if (!Field.IsValid() || !Field->TryGetStringField(TEXT("field_name"), Name) || !Name.Equals(Category, ESearchCase::CaseSensitive)
				|| !Field->TryGetArrayField(TEXT("value"), Codes) || Codes == nullptr)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& CodeElement : *Codes)
			{
				const TSharedPtr<FJsonObject> CodeField = FFlockJsonUtils::AsObjectOrNull(CodeElement);
				FString Code;
				if (CodeField.IsValid() && CodeField->TryGetStringField(TEXT("field_name"), Code))
				{
					AddBalance(Code, CodeField->TryGetField(TEXT("value")), OutBalances);
				}
			}
			return;
		}
	}

	/** One wallet category as a plain object of codes to whole numbers (an older row's form, or the flattened data). */
	void ReadObject(const TSharedPtr<FJsonObject>& Data, const FString& Category, TArray<FFlockCurrencyBalance>& OutBalances)
	{
		const TSharedPtr<FJsonObject>* Codes = nullptr;
		if (Data.IsValid() && Data->TryGetObjectField(Category, Codes) && Codes != nullptr && Codes->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Code : (*Codes)->Values)
			{
				AddBalance(Code.Key, Code.Value, OutBalances);
			}
		}
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		if (!Json.IsEmpty())
		{
			const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Json);
			FJsonSerializer::Deserialize(Reader, Object);
		}
		return Object;
	}

	const FFlockCurrencyBalance* FindBalance(const TArray<FFlockCurrencyBalance>& Balances, const FString& CurrencyCode)
	{
		return Balances.FindByPredicate([&CurrencyCode](const FFlockCurrencyBalance& Balance)
		{
			return Balance.Code.Equals(CurrencyCode, ESearchCase::CaseSensitive);
		});
	}
}

int64 FFlockWallet::BalanceOf(const FString& CurrencyCode) const
{
	if (const FFlockCurrencyBalance* Game = FindBalance(GameCurrencies, CurrencyCode))
	{
		return Game->Amount;
	}
	const FFlockCurrencyBalance* Real = FindBalance(RealCurrencies, CurrencyCode);
	return Real ? Real->Amount : 0;
}

bool FFlockWallet::Holds(const FString& CurrencyCode) const
{
	return !CurrencyCode.IsEmpty() && (FindBalance(GameCurrencies, CurrencyCode) || FindBalance(RealCurrencies, CurrencyCode));
}

bool FFlockWallet::CanAfford(const FFlockShopItem& Item) const
{
	if (Item.IsRealMoney || Item.Currency.IsEmpty())
	{
		return false;
	}
	const FFlockCurrencyBalance* Balance = FindBalance(GameCurrencies, Item.Currency);
	return Balance && Balance->Amount >= Item.Price;
}

FFlockWallet FFlockWallet::From(const FFlockPlayerData& Wallet)
{
	FFlockWallet Result;
	// The server's own node keeps each currency code exactly as configured; the flattened data recases field names.
	const TSharedPtr<FJsonObject> Wire = ParseObject(Wallet.Data.WireJson);
	const TSharedPtr<FJsonValue> Data = Wire.IsValid() ? Wire->TryGetField(TEXT("data")) : nullptr;
	if (Data.IsValid() && Data->Type == EJson::Array)
	{
		ReadFieldList(Data->AsArray(), TEXT("game_currencies"), Result.GameCurrencies);
		ReadFieldList(Data->AsArray(), TEXT("real_currencies"), Result.RealCurrencies);
		return Result;
	}
	if (Data.IsValid() && Data->Type == EJson::Object)
	{
		ReadObject(Data->AsObject(), TEXT("game_currencies"), Result.GameCurrencies);
		ReadObject(Data->AsObject(), TEXT("real_currencies"), Result.RealCurrencies);
		return Result;
	}
	// No server node: the row holds fields the game wrote while offline, laid over it. Read the flattened form, whose codes
	// keep their case unless written in lower case or with underscores.
	const TSharedPtr<FJsonObject> Flat = ParseObject(Wallet.Data.ToJsonString());
	ReadObject(Flat, TEXT("GameCurrencies"), Result.GameCurrencies);
	ReadObject(Flat, TEXT("RealCurrencies"), Result.RealCurrencies);
	return Result;
}

// ─────────────────────────────── Owned items ───────────────────────────────

int32 FFlockOwnedItemCounts::CountOf(const FString& ShopItemId) const
{
	const FFlockOwnedItemCount* Row = Items.FindByPredicate([&ShopItemId](const FFlockOwnedItemCount& Held)
	{
		return Held.ShopItemId.Equals(ShopItemId, ESearchCase::CaseSensitive);
	});
	return Row ? Row->Count : 0;
}
