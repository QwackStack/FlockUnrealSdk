// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Models/FlockAssetModels.h"
#include "FlockAssetLibrary.generated.h"

/**
 * The synchronous half of the asset surface: cache questions that need no network and therefore no async
 * node. Each resolves the SDK from its calling graph, so no Target pin is needed, and each is a safe
 * no-op returning a default when the SDK isn't up — same contract as UFlockLibrary.
 *
 * The download and fetch calls are async nodes instead; see FlockAssetAsyncActions.h.
 */
UCLASS()
class FLOCK_API UFlockAssetLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Whether this exact version of the asset is already on disk. Does not download and does not stamp LRU. */
	UFUNCTION(BlueprintPure, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Is Asset Cached"),
		Category = "Flock|Assets")
	static bool IsAssetCached(UObject* WorldContextObject, const FFlockAsset& Asset);

	/** Those of Assets not already on disk — what a preload would actually fetch. */
	UFUNCTION(BlueprintPure, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Uncached Assets"),
		Category = "Flock|Assets")
	static TArray<FFlockAsset> GetUncachedAssets(UObject* WorldContextObject, const TArray<FFlockAsset>& Assets);

	/** The cached file path for this exact version, or empty when it isn't cached. Never downloads. */
	UFUNCTION(BlueprintPure, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Cached Asset Path"),
		Category = "Flock|Assets")
	static FString GetCachedAssetPath(UObject* WorldContextObject, const FFlockAsset& Asset);

	/** Where the binary asset cache lives on this device. */
	UFUNCTION(BlueprintPure, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Get Asset Cache Directory"),
		Category = "Flock|Assets")
	static FString GetAssetCacheDirectory(UObject* WorldContextObject);

	/** Drops every cached asset file, the in-process index, and the cached asset list. */
	UFUNCTION(BlueprintCallable, meta = (WorldContext = "WorldContextObject", DisplayName = "Flock Clear Asset Cache"),
		Category = "Flock|Assets")
	static void ClearAssetCache(UObject* WorldContextObject);
};
