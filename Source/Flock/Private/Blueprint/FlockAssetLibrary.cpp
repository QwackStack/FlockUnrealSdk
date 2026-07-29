// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Blueprint/FlockAssetLibrary.h"

#include "FlockSubsystem.h"
#include "Providers/FlockAssetProvider.h"

namespace
{
	FFlockAssetProvider* Assets(UObject* WorldContextObject)
	{
		UFlockSubsystem* Sdk = UFlockSubsystem::Get(WorldContextObject);
		return Sdk ? Sdk->GetAssetProvider() : nullptr;
	}
}

bool UFlockAssetLibrary::IsAssetCached(UObject* WorldContextObject, const FFlockAsset& Asset)
{
	const FFlockAssetProvider* Provider = Assets(WorldContextObject);
	return Provider ? Provider->IsCached(Asset) : false;
}

TArray<FFlockAsset> UFlockAssetLibrary::GetUncachedAssets(UObject* WorldContextObject, const TArray<FFlockAsset>& InAssets)
{
	const FFlockAssetProvider* Provider = Assets(WorldContextObject);
	// Without the SDK nothing can be cached, so everything asked about is still outstanding.
	return Provider ? Provider->GetUncached(InAssets) : InAssets;
}

FString UFlockAssetLibrary::GetCachedAssetPath(UObject* WorldContextObject, const FFlockAsset& Asset)
{
	const FFlockAssetProvider* Provider = Assets(WorldContextObject);
	return Provider ? Provider->GetCachedFilePath(Asset) : FString();
}

FString UFlockAssetLibrary::GetAssetCacheDirectory(UObject* WorldContextObject)
{
	const FFlockAssetProvider* Provider = Assets(WorldContextObject);
	return Provider ? Provider->GetCacheDirectory() : FString();
}

void UFlockAssetLibrary::ClearAssetCache(UObject* WorldContextObject)
{
	if (FFlockAssetProvider* Provider = Assets(WorldContextObject))
	{
		Provider->ClearCache();
	}
}
