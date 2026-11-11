// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "FlockSubsystem.h"
#include "Http/FlockError.h"
#include "Http/FlockErrorHints.h"
#include "Providers/FlockShopProvider.h"
#include "Store/FlockStore.h"

/**
 * What the shop and store Blueprint nodes share: finding Flock's shop calls and its connected store, and the failures for
 * neither being there. One wording each, whichever node gives it.
 */
namespace FlockBlueprintShop
{
	/** A node found no Flock (no World Context in a game's world, or Flock not initialized): what is wrong and how to initialize it. */
	inline FFlockError NoFlock()
	{
		return FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("Flock shop is not available: Flock is not initialized"),
			FFlockErrorHints::InitializeFlockFirst());
	}

	/** A store node with no store connected: what is wrong and how to connect one. */
	inline FFlockError NoStoreConnected()
	{
		return FFlockError::MakeWithFix(EFlockErrorType::Validation, TEXT("No store is connected"), FFlockErrorHints::ConnectAStoreFirst());
	}

	/** Flock's shop calls, or null with OutError saying how to initialize Flock. */
	inline FFlockShopProvider* ShopOf(UFlockSubsystem* Flock, FFlockError& OutError)
	{
		FFlockShopProvider* Shop = Flock != nullptr ? Flock->GetShopProvider() : nullptr;
		if (Shop == nullptr)
		{
			OutError = NoFlock();
		}
		return Shop;
	}

	/** The store Flock holds, or null with OutError saying why (no Flock, or no store connected). */
	inline UFlockStore* StoreOf(UFlockSubsystem* Flock, FFlockError& OutError)
	{
		if (Flock == nullptr)
		{
			OutError = NoFlock();
			return nullptr;
		}
		UFlockStore* Store = Flock->GetStore();
		if (Store == nullptr)
		{
			OutError = NoStoreConnected();
		}
		return Store;
	}
}
