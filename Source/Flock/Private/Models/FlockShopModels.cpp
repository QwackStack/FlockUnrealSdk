// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Models/FlockShopModels.h"

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
	Object->TryGetStringField(TEXT("created_at"), OutStruct.CreatedAt);
	Object->TryGetStringField(TEXT("updated_at"), OutStruct.UpdatedAt);

	// `data` is an open dict — kept verbatim inside the handle rather than routed through the wire transform.
	OutStruct.Data = FFlockJsonData::FromJson(Object->TryGetField(TEXT("data")));
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
