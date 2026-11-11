// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockStoreLibrary.h"
#include "Blueprint/FlockBlueprintShop.h"

#include "FlockSubsystem.h"

bool UFlockStoreLibrary::GetStorePrice(UObject* WorldContextObject, const FString& ShopItemId, FFlockStorePrice& Price, FFlockError& Error)
{
	return GetStorePriceWithFlockForTesting(UFlockSubsystem::Get(WorldContextObject), ShopItemId, Price, Error);
}

bool UFlockStoreLibrary::GetStorePriceWithFlockForTesting(UFlockSubsystem* Flock, const FString& ShopItemId, FFlockStorePrice& Price, FFlockError& Error)
{
	Price = FFlockStorePrice();
	Error = FFlockError();
	UFlockStore* Store = FlockBlueprintShop::StoreOf(Flock, Error);
	if (Store == nullptr)
	{
		return false;
	}
	const TFlockResult<FFlockStorePrice> Result = Store->GetPrice(ShopItemId);
	if (!Result.IsSuccess())
	{
		Error = Result.Error;
		return false;
	}
	Price = Result.Value;
	return true;
}

UFlockStore* UFlockStoreLibrary::GetStore(UObject* WorldContextObject)
{
	return GetStoreWithFlockForTesting(UFlockSubsystem::Get(WorldContextObject));
}

UFlockStore* UFlockStoreLibrary::GetStoreWithFlockForTesting(const UFlockSubsystem* Flock)
{
	return Flock != nullptr ? Flock->GetStore() : nullptr;
}

void UFlockStoreLibrary::DisconnectStore(UObject* WorldContextObject)
{
	DisconnectStoreWithFlockForTesting(UFlockSubsystem::Get(WorldContextObject));
}

void UFlockStoreLibrary::DisconnectStoreWithFlockForTesting(const UFlockSubsystem* Flock)
{
	if (UFlockStore* Store = GetStoreWithFlockForTesting(Flock))
	{
		Store->Disconnect();
	}
}
