// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockConfigModels.h"
#include "Models/FlockGameModels.h"
#include "FlockConfigAsyncActions.generated.h"

/**
 * Blueprint async nodes for the config and game fetches. Each resolves the SDK from its world context on
 * Activate and fires exactly one pin; when the SDK is unavailable the failure pin fires with a Validation
 * error. This surface is wider than the C++ provider's "public" intent on purpose — without codegen a graph
 * needs by-name / by-tag fetches to read config at all.
 *
 * Read values off a returned FFlockStructuredData with UFlockStructuredDataLibrary.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockConfigPin, const FFlockGameConfigSchema&, Config, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockConfigListPin, const TArray<FFlockGameConfigSchema>&, Configs, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockConfigDataPin, const FFlockStructuredData&, Data, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockGamePin, const FFlockGameSchema&, Game, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockGameVersionPin, const FFlockGameVersionSchema&, Version, const FFlockError&, Error);

/** Fetches a single config by name or by id. */
UCLASS()
class FLOCK_API UFlockGetConfigAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockConfigPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockConfigPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Config By Name"), Category = "Flock|Config")
	static UFlockGetConfigAction* GetConfigByName(UObject* WorldContextObject, const FString& Name);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Config By Id"), Category = "Flock|Config")
	static UFlockGetConfigAction* GetConfigById(UObject* WorldContextObject, const FString& ConfigId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockGameConfigSchema>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	bool bByName = true;
	FString Argument;
};

/** Fetches configs filtered by tag (Any = all). */
UCLASS()
class FLOCK_API UFlockGetConfigsByTagAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockConfigListPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockConfigListPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Configs By Tag"), Category = "Flock|Config")
	static UFlockGetConfigsByTagAction* GetConfigsByTag(UObject* WorldContextObject, EFlockConfigTag Tag);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<TArray<FFlockGameConfigSchema>>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	EFlockConfigTag Tag = EFlockConfigTag::Any;
};

/** Resolves a config's effective data: the patch for this game version if present, else the config's base data. */
UCLASS()
class FLOCK_API UFlockResolveConfigDataAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockConfigDataPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockConfigDataPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Resolve Config Data"), Category = "Flock|Config")
	static UFlockResolveConfigDataAction* ResolveConfigData(UObject* WorldContextObject, const FString& ConfigId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockStructuredData>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString ConfigId;
};

/** Fetches the game record. */
UCLASS()
class FLOCK_API UFlockGetGameAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockGamePin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGamePin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Game"), Category = "Flock|Game")
	static UFlockGetGameAction* GetGame(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockGameSchema>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};

/**
 * Fetches a player's resolved feature flags (empty Player Id = the signed-in player). Never cached
 * across calls the way a config is — features are re-resolved rather than served stale.
 */
UCLASS()
class FLOCK_API UFlockGetPlayerFeaturesAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockConfigPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockConfigPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Player Features", AdvancedDisplay = "PlayerId"), Category = "Flock|Config")
	static UFlockGetPlayerFeaturesAction* GetPlayerFeatures(UObject* WorldContextObject, const FString& PlayerId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockGameConfigSchema>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString PlayerId;
};

/** Fetches a game version — this build's version, or one looked up by name. */
UCLASS()
class FLOCK_API UFlockGetGameVersionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockGameVersionPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockGameVersionPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Game Version"), Category = "Flock|Game")
	static UFlockGetGameVersionAction* GetGameVersion(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Get Game Version By Name"), Category = "Flock|Game")
	static UFlockGetGameVersionAction* GetGameVersionByName(UObject* WorldContextObject, const FString& Name);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockGameVersionSchema>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	bool bByName = false;
	FString Name;
};
