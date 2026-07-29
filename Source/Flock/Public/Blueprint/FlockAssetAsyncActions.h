// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockAssetModels.h"
#include "FlockAssetAsyncActions.generated.h"

class UTexture2D;

/**
 * Blueprint async nodes for assets. Each resolves the SDK from its world context on Activate; when the
 * SDK is unavailable the failure pin fires with a Validation error.
 *
 * **The download nodes take one string pin — an asset name or an id, whichever the graph has.** Names
 * are what a designer actually knows, and a graph cannot pick between the two at design time, so the
 * provider resolves it (ids win, then names). That is the whole ergonomic point of this surface: one
 * node, one pin, a texture out.
 *
 * **These are the one place in the SDK where a pin fires more than once.** Every other async node fires
 * exactly one pin exactly once; a download also has an On Progress pin that fires repeatedly while bytes
 * arrive. Success and Failure keep the usual contract — exactly one of them, once, at the end.
 *
 * Bytes Total is 0 until the server declares a Content-Length, so Progress stays 0 for the first tick or
 * two. Drive an indeterminate bar off that rather than treating it as "no progress".
 */

// Each node uses one pin shape for all three of its pins, so Success, Failure and Progress look alike in
// a graph. On Progress only Progress is meaningful; on Success it is 1.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFlockAssetTexturePin, UTexture2D*, Texture, float, Progress, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFlockAssetTextPin, const FString&, Text, float, Progress, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFlockAssetBytesPin, const TArray<uint8>&, Bytes, float, Progress, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FFlockAssetFilePin, const FString&, FilePath, float, Progress, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockAssetPin, const FFlockAsset&, Asset, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockAssetListPin, const TArray<FFlockAsset>&, Assets, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FFlockAssetPreloadPin, int32, Succeeded, int32, Requested, float, Progress, const FFlockError&, Error);

/** Downloads an asset and hands back a texture. png/jpg/bmp/exr/tga — whatever the engine can decode. */
UCLASS()
class FLOCK_API UFlockDownloadAssetTextureAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAssetTexturePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetTexturePin OnFailure;

	/** Fires repeatedly while bytes arrive. Progress is 0 until the server reports a content length. */
	UPROPERTY(BlueprintAssignable)
	FFlockAssetTexturePin OnProgress;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Download Asset Texture"), Category = "Flock|Assets")
	static UFlockDownloadAssetTextureAction* DownloadAssetTexture(UObject* WorldContextObject, const FString& Asset);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	/** Holds a reference the instant the texture exists, so it cannot be collected before it is broadcast. */
	UPROPERTY()
	TObjectPtr<UTexture2D> Downloaded;

	FString Asset;
};

/** Downloads an asset and decodes it as UTF-8 text — JSON, CSV, dialogue tables. */
UCLASS()
class FLOCK_API UFlockDownloadAssetTextAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAssetTextPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetTextPin OnFailure;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetTextPin OnProgress;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Download Asset Text"), Category = "Flock|Assets")
	static UFlockDownloadAssetTextAction* DownloadAssetText(UObject* WorldContextObject, const FString& Asset);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString Asset;
};

/** Downloads an asset as raw bytes. */
UCLASS()
class FLOCK_API UFlockDownloadAssetBytesAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAssetBytesPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetBytesPin OnFailure;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetBytesPin OnProgress;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Download Asset Bytes"), Category = "Flock|Assets")
	static UFlockDownloadAssetBytesAction* DownloadAssetBytes(UObject* WorldContextObject, const FString& Asset);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString Asset;
};

/**
 * Downloads an asset and hands back its local file path — the escape hatch for everything the engine
 * cannot decode from bytes. Audio in particular: UE has no API that turns mp3 or ogg into a USoundWave,
 * so route the path to whatever imports audio in your project rather than expecting a sound pin here.
 *
 * With the asset cache enabled the path is inside the cache and outlives the call. With it disabled the
 * file is scratch that the caller owns and should delete.
 */
UCLASS()
class FLOCK_API UFlockDownloadAssetFileAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAssetFilePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetFilePin OnFailure;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetFilePin OnProgress;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Download Asset File"), Category = "Flock|Assets")
	static UFlockDownloadAssetFileAction* DownloadAssetFile(UObject* WorldContextObject, const FString& Asset);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString Asset;
};

/** Every asset record for this game version. Snapshot-backed, so it still answers offline. */
UCLASS()
class FLOCK_API UFlockGetAssetsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAssetListPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetListPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Assets"), Category = "Flock|Assets")
	static UFlockGetAssetsAction* GetAssets(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/** One asset record, by name or id. */
UCLASS()
class FLOCK_API UFlockGetAssetAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAssetPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Asset"), Category = "Flock|Assets")
	static UFlockGetAssetAction* GetAsset(UObject* WorldContextObject, const FString& Asset);

	virtual void Activate() override;

private:
	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString Asset;
};

/**
 * Warms the cache for a set of assets, honouring the concurrency cap — the loading-screen node.
 *
 * Individual failures do not fail the batch: Succeeded reports how many landed out of Requested, and the
 * success pin fires as long as the batch ran. One unreachable asset should not cost the other forty.
 */
UCLASS()
class FLOCK_API UFlockPreloadAssetsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockAssetPreloadPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetPreloadPin OnFailure;

	UPROPERTY(BlueprintAssignable)
	FFlockAssetPreloadPin OnProgress;

	/** Preloads the given records — pair it with Flock Get Assets and filter in the graph. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Preload Assets"), Category = "Flock|Assets")
	static UFlockPreloadAssetsAction* PreloadAssets(UObject* WorldContextObject, const TArray<FFlockAsset>& Assets);

	/** Preloads every asset for this game version. */
	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Preload All Assets"), Category = "Flock|Assets")
	static UFlockPreloadAssetsAction* PreloadAllAssets(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<int32>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	TArray<FFlockAsset> Assets;
	bool bAll = false;
};
