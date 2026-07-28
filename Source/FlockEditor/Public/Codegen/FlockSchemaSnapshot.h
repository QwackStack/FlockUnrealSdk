// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Models/FlockConfigModels.h"
#include "Models/FlockPlayerModels.h"
#include "Models/FlockShopModels.h"

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

	/** True when the resolved and baked version ids disagree — see BakedGameVersionId. */
	bool IsBakeStale() const
	{
		return !BakedGameVersionId.IsEmpty() && !GameVersionId.IsEmpty() && BakedGameVersionId != GameVersionId;
	}
};
