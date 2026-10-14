// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockConfigModels.h"
#include "Models/FlockPlayerModels.h"
#include "Models/FlockShopModels.h"

/**
 * Id, then name, letter for letter: the one order codegen walks entities in. Generated names (where two
 * entities would share one, the later takes `_2`) and the content hash then never depend on the order
 * the server listed them in.
 */
template <typename T>
bool FlockCodegenComesBefore(const T& A, const T& B)
{
	const int32 ById = A.Id.Compare(B.Id, ESearchCase::CaseSensitive);
	return ById != 0 ? ById < 0 : A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
}

/**
 * Everything codegen emits from, fetched in one pass: the player templates and game configs whose
 * `schema` describes the typed shapes, and the shops whose items and currencies become typed ids.
 *
 * A snapshot is all-or-nothing. A partial one is worse than none — emitters wipe their output before
 * writing, so a half-fetched snapshot would delete a working generated surface and replace it with less.
 * The fetcher therefore never hands back a partial result; it fails instead.
 *
 * Editor-only, and deliberately not a USTRUCT: nothing serializes it, nothing reflects over it, and it
 * holds runtime models that already carry their own wire parsing.
 */
struct FLOCKEDITOR_API FFlockSchemaSnapshot
{
	/** The version the snapshot describes, resolved from the configured version *name* at fetch time. */
	FString GameVersionId;

	/**
	 * The id currently baked into project settings. Normally identical to GameVersionId; a mismatch means
	 * the bake is stale (the backend cut a new version for the same name), which is worth surfacing rather
	 * than silently generating against one id while the game runs against another.
	 */
	FString BakedGameVersionId;

	FDateTime FetchedAtUtc;

	TArray<FFlockPlayerTemplateSchema> PlayerTemplates;
	TArray<FFlockGameConfigSchema> GameConfigs;
	TArray<FFlockShop> Shops;

	/** True when the resolved and baked version ids disagree — see BakedGameVersionId. Compared letter for letter. */
	bool IsBakeStale() const
	{
		return !BakedGameVersionId.IsEmpty() && !GameVersionId.IsEmpty()
			&& !BakedGameVersionId.Equals(GameVersionId, ESearchCase::CaseSensitive);
	}

	/** Puts templates, configs, shops and each shop's items in FlockCodegenComesBefore order. */
	void SortForEmitting()
	{
		PlayerTemplates.StableSort([](const FFlockPlayerTemplateSchema& A, const FFlockPlayerTemplateSchema& B) { return FlockCodegenComesBefore(A, B); });
		GameConfigs.StableSort([](const FFlockGameConfigSchema& A, const FFlockGameConfigSchema& B) { return FlockCodegenComesBefore(A, B); });
		Shops.StableSort([](const FFlockShop& A, const FFlockShop& B) { return FlockCodegenComesBefore(A, B); });
		for (FFlockShop& Shop : Shops)
		{
			Shop.ShopItems.StableSort([](const FFlockShopItem& A, const FFlockShopItem& B) { return FlockCodegenComesBefore(A, B); });
		}
	}
};
