// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#pragma once

#include "CoreMinimal.h"
#include "Http/FlockError.h"
#include "Http/FlockResult.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Models/FlockCommandModels.h"
#include "Models/FlockPlayerModels.h"
#include "FlockCommandAsyncActions.generated.h"

/**
 * Blueprint async nodes for game commands. Each resolves the SDK from its world context on Activate and
 * fires exactly one pin; when the SDK is unavailable the failure pin fires with a Validation error.
 *
 * Every command answers with the whole updated player-data row — read its values with the Flock data
 * library nodes. Build the values to write with UFlockCommandDataLibrary's Set Command / Command Value
 * nodes.
 *
 * Offline behavior differs by command, deliberately. Update Player Data / Update Player Data Field /
 * Unlock Achievement queue and succeed with the optimistically-updated row, replaying when connectivity
 * returns. **Add Game Funds fails instead**: a money grant is never queued and never re-sent after an
 * ambiguous failure, so it can't double-credit.
 */

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockCommandResultPin, const FFlockPlayerData&, Data, const FFlockError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFlockCommandFlushPin, int32, DeliveredCount, const FFlockError&, Error);

/** Writes a set of fields onto a player-data row, or a single field onto one. */
UCLASS()
class FLOCK_API UFlockUpdatePlayerDataAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockCommandResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockCommandResultPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Update Player Data"), Category = "Flock|Commands")
	static UFlockUpdatePlayerDataAction* UpdatePlayerData(UObject* WorldContextObject, const FString& PlayerDataId,
		const FFlockCommandData& Data);

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Update Player Data Field"), Category = "Flock|Commands")
	static UFlockUpdatePlayerDataAction* UpdatePlayerDataField(UObject* WorldContextObject, const FString& PlayerDataId,
		const FString& Key, const FFlockCommandValue& Value);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerData>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	// A single-field write is the whole-bag write with a key set; the empty key is what selects the route.
	FString PlayerDataId;
	FString Key;
	FFlockCommandData Data;
	FFlockCommandValue Value;
};

/** Unlocks an achievement on the signed-in player's achievements row (resolved by the "achievement" tag). */
UCLASS()
class FLOCK_API UFlockUnlockAchievementAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockCommandResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockCommandResultPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Unlock Achievement"), Category = "Flock|Commands")
	static UFlockUnlockAchievementAction* UnlockAchievement(UObject* WorldContextObject, const FString& AchievementName);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerData>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString AchievementName;
};

/**
 * Adds funds to the signed-in player's wallet. Leave Currency Template Id empty to resolve the
 * "currency"-tagged template at runtime; pass a known id to skip that lookup.
 *
 * Fails rather than queueing when offline, and is never re-sent after an ambiguous failure.
 */
UCLASS()
class FLOCK_API UFlockAddGameFundsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockCommandResultPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockCommandResultPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Add Game Funds"), Category = "Flock|Commands")
	static UFlockAddGameFundsAction* AddGameFunds(UObject* WorldContextObject, const FString& Currency, int32 Amount,
		const FString& CurrencyTemplateId);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<FFlockPlayerData>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;

	FString Currency;
	int32 Amount = 0;
	FString CurrencyTemplateId;
};

/**
 * Replays the signed-in player's queued offline commands, reporting how many were delivered. The SDK
 * already flushes on sign-in, on returning to the foreground, and when connectivity comes back — this is
 * for a manual "retry sync" button.
 */
UCLASS()
class FLOCK_API UFlockFlushPendingCommandsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable)
	FFlockCommandFlushPin OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FFlockCommandFlushPin OnFailure;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
		DisplayName = "Flock Flush Pending Commands"), Category = "Flock|Commands")
	static UFlockFlushPendingCommandsAction* FlushPendingCommands(UObject* WorldContextObject);

	virtual void Activate() override;

private:
	void Complete(const TFlockResult<int32>& Result);

	UPROPERTY()
	TObjectPtr<UObject> WorldContextObject;
};
